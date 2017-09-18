/** \file    zts_client.cc
    \brief   Downloads bibliographic metadata using a Zotero Translation server.
    \author  Dr. Johannes Ruscheinski

    \copyright 2017, Library of the University of Tübingen

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <iostream>
#include <unordered_map>
#include <cinttypes>
#include <uuid/uuid.h>
#include "Compiler.h"
#include "ExecUtil.h"
#include "FileDescriptor.h"
#include "FileUtil.h"
#include "HttpHeader.h"
#include "JSON.h"
#include "MarcRecord.h"
#include "MarcWriter.h"
#include "MiscUtil.h"
#include "RegexMatcher.h"
#include "SocketUtil.h"
#include "StringUtil.h"
#include "TextUtil.h"
#include "UrlUtil.h"
#include "util.h"
#include "WebUtil.h"


const std::string DEFAULT_ZOTOERO_CRAWLER_CONFIG_PATH("/usr/local/var/lib/tuelib/zotero_crawler.conf");


void Usage() {
    std::cerr << "Usage: " << ::progname
              << " [--ignore-robots-dot-txt] [--zotero-crawler-config-file=path] [--progress-file=progress_filename] zts_server_url map_directory marc_output\n"
              << "        Where \"map_directory\" is a path to a subdirectory containing all required map\n"
              << "        files and the file containing hashes of previously generated records.\n"
              << "        The optional \"--zotero-crawler-config-file\" flag specifies where to look for the\n"
              << "        config file for the \"zotero_crawler\", the default being\n"
              << "        " << DEFAULT_ZOTOERO_CRAWLER_CONFIG_PATH << ".\n\n";
    std::exit(EXIT_FAILURE);
}


bool ParseLine(const std::string &line, std::string * const key, std::string * const value) {
    key->clear(), value->clear();

    // Extract the key:
    auto ch(line.cbegin());
    while (ch != line.cend() and *ch != '=') {
        if (unlikely(*ch == '\\')) {
            ++ch;
            if (unlikely(ch == line.cend()))
                return false;
        }
        *key += *ch++;
    }
    if (unlikely(ch == line.cend()))
        return false;
    ++ch; // Skip over the equal-sign.

    // Extract value:
    while (ch != line.cend() and *ch != '#' /* Comment start. */) {
        if (unlikely(*ch == '\\')) {
            ++ch;
            if (unlikely(ch == line.cend()))
                return false;
        }
        *value += *ch++;
    }
    StringUtil::RightTrim(value);

    return not key->empty() and not value->empty();
}


void LoadMapFile(const std::string &filename, std::unordered_map<std::string, std::string> * const from_to_map) {
    std::unique_ptr<File> input(FileUtil::OpenInputFileOrDie(filename));

    unsigned line_no(0);
    while (not input->eof()) {
        std::string line(input->getline());
        ++line_no;

        StringUtil::Trim(&line);
        std::string key, value;
        if (not ParseLine(line, &key, &value))
            Error("in LoadMapFile: invalid input on line \"" + std::to_string(line_no) + "\" in \""
                  + input->getPath() + "\"!");
        from_to_map->emplace(key, value);
    }
}


RegexMatcher *LoadSupportedURLsRegex(const std::string &map_directory_path) {
    std::unique_ptr<File> input(FileUtil::OpenInputFileOrDie(map_directory_path + "targets.regex"));

    std::string combined_regex;
    while (not input->eof()) {
        std::string line(input->getline());
        StringUtil::Trim(&line);
        if (likely(not line.empty())) {
            if (likely(not combined_regex.empty()))
                combined_regex += '|';
            combined_regex += "(?:" + line + ")";
        }
    }

    std::string err_msg;
    RegexMatcher * const supported_urls_regex(RegexMatcher::RegexMatcherFactory(combined_regex, &err_msg));
    if (supported_urls_regex == nullptr)
        Error("in LoadSupportedURLsRegex: compilation of the combined regex failed: " + err_msg);

    return supported_urls_regex;
}


void LoadPreviouslyDownloadedHashes(File * const input,
                                    std::unordered_set<std::string> * const previously_downloaded)
{
    while (not input->eof()) {
        std::string line(input->getline());
        StringUtil::Trim(&line);
        if (likely(not line.empty()))
            previously_downloaded->emplace(TextUtil::Base64Decode(line));
    }

    std::cerr << "Loaded " << previously_downloaded->size() << " hashes of previously generated records.\n";
}


// Returns the socket file descriptor on success or aborts if it was not possible to establish a connection.
int TcpConnectOrDie(const std::string &server_address, const unsigned short server_port, const TimeLimit &time_limit)
{
   std::string tcp_connect_error_message;
   const int socket_fd(SocketUtil::TcpConnect(server_address, server_port, time_limit, &tcp_connect_error_message,
                                              SocketUtil::DISABLE_NAGLE));
   if (socket_fd == -1)
       Error("in TcpConnectOrDie: Could not open TCP connection to " + server_address + ", port "
             + std::to_string(server_port) + ": " + tcp_connect_error_message + " (Time remaining: "
             + std::to_string(time_limit.getRemainingTime()) + ").");

   return socket_fd;
}


bool Download(const std::string &server_address, const unsigned short server_port, const std::string &server_path,
              const TimeLimit &time_limit, const std::string &request_headers, const std::string &request_body,
              std::string * const returned_data, std::string * const error_message)
{
    returned_data->clear();
    error_message->clear();

    const FileDescriptor socket_fd(TcpConnectOrDie(server_address, server_port, time_limit));

    std::string request;
    request += request_headers;
    request += "\r\n";
    request += request_body;

    if (SocketUtil::TimedWrite(socket_fd, time_limit, request.data(), request.length()) == -1) {
        *error_message = "Could not write to socket";
        *error_message += " (Time remaining: " + std::to_string(time_limit.getRemainingTime()) + ")";
        *error_message += '!';
        return false;
    }

    char http_response_header[10240 + 1];
    ssize_t no_of_bytes_read(SocketUtil::TimedRead(socket_fd, time_limit, http_response_header,
                                                   sizeof(http_response_header) - 1));
    if (no_of_bytes_read == -1) {
        *error_message = "Could not read from socket (1).";
        *error_message += " (Time remaining: " + std::to_string(time_limit.getRemainingTime()) + ").";
        return false;
    }
    http_response_header[no_of_bytes_read] = '\0';
    HttpHeader http_header(http_response_header);

    // the 2xx codes indicate success:
    if (http_header.getStatusCode() < 200 or http_header.getStatusCode() > 299) {
        *error_message = "Web server returned error status code ("
                         + std::to_string(http_header.getStatusCode()) + "), address was "
                         + server_address + ", port was " + std::to_string(server_port) + ", path was \""
                         + server_path +"\"!";
        return false;
    }

    // read the returned document source:
    std::string response(http_response_header, no_of_bytes_read);
    char buf[10240];
    do {
        no_of_bytes_read = SocketUtil::TimedRead(socket_fd, time_limit, buf, sizeof(buf));
        if (no_of_bytes_read == -1) {
            *error_message = "Could not read from socket (2).";
            *error_message += " (Time remaining: " + std::to_string(time_limit.getRemainingTime()) + ").";
            return false;
        }
        if (no_of_bytes_read > 0)
            response.append(buf, no_of_bytes_read);
    } while (no_of_bytes_read > 0);

    std::string::size_type pos(response.find("\r\n\r\n")); // the header ends with two cr/lf pairs!
    if (pos != std::string::npos) {
        pos += 4;
        *returned_data = response.substr(pos);
    }

    return true;
}


// We try to be unique for the machine we're on.  Beyond that we may have a problem.
std::string GetNextSessionId() {
    static unsigned counter;
    static uint32_t uuid[4];
    if (unlikely(counter == 0))
        ::uuid_generate(reinterpret_cast<unsigned char *>(uuid));
    ++counter;
    return "ub_tools_zts_client_" + StringUtil::ToString(uuid[0]) + StringUtil::ToString(uuid[1])
           + StringUtil::ToString(uuid[2]) + StringUtil::ToString(uuid[3]) + "_" + StringUtil::ToString(counter);
}


bool Download(const std::string &server_address, const unsigned short server_port, const std::string &server_path,
              const TimeLimit &time_limit, const std::string &harvest_url, std::string * const json_result,
              std::string * const error_message)
{
        const std::string JSON_REQUEST("{\"url\":\"" + harvest_url + "\",\"sessionid\":\"" + GetNextSessionId()
                                       + "\"}");
        std::string headers;
        headers += "POST " + server_path + " HTTP/1.0\r\n";
        headers += "Host: " + server_address + "\r\n";
        headers += "User-Agent: zts_client/1.0 ub_tools\r\n";
        headers += "Accept: application/json\r\n";
        headers += "Connection: close\r\n";
        headers += "Content-Type: application/json\r\n";
        headers += "Content-Length: " + std::to_string(JSON_REQUEST.length()) + "\r\n";

        return Download(server_address, server_port, server_path, time_limit, headers, JSON_REQUEST, json_result,
                        error_message);
}


inline bool Download(const Url &url, const TimeLimit &time_limit, const std::string &harvest_url,
                     std::string * const json_result, std::string * const error_message)
{
    return Download(url.getAuthority(), url.getPort(), url.getPath(), time_limit, harvest_url, json_result,
                    error_message);
}


inline std::string GetValueFromStringNode(const std::pair<std::string, JSON::JSONNode *> &key_and_node) {
    if (key_and_node.second->getType() != JSON::JSONNode::STRING_NODE)
        Error("in GetValueFromStringNode: expected \"" + key_and_node.first + "\" to have a string node!");
    const JSON::StringNode * const node(reinterpret_cast<const JSON::StringNode * const>(key_and_node.second));
    return node->getValue();
}


inline std::string CreateSubfieldFromStringNode(const std::string &key, const JSON::JSONNode * const node,
                                                const std::string &tag, const char subfield_code,
                                                MarcRecord * const marc_record, const char indicator1 = ' ',
                                                const char indicator2 = ' ')
{
    if (node->getType() != JSON::JSONNode::STRING_NODE)
        Error("in CreateSubfieldFromStringNode: \"" + key + "\" is not a string node!");
    const std::string value(reinterpret_cast<const JSON::StringNode * const>(node)->getValue());
    marc_record->insertSubfield(tag, subfield_code, value, indicator1, indicator2);
    return value;
}


inline std::string CreateSubfieldFromStringNode(const std::pair<std::string, JSON::JSONNode *> &key_and_node,
                                                const std::string &tag, const char subfield_code,
                                                MarcRecord * const marc_record, const char indicator1 = ' ',
                                                const char indicator2 = ' ')
{
    return CreateSubfieldFromStringNode(key_and_node.first, key_and_node.second, tag, subfield_code, marc_record,
                                        indicator1, indicator2);
}


// Returns the value for "key", if key exists in "object", o/w returns the empty string.
inline std::string GetOptionalStringValue(const JSON::ObjectNode &object, const std::string &key) {
    const JSON::JSONNode * const value_node(object.getValue(key));
    if (value_node == nullptr)
        return "";

    if (value_node->getType() != JSON::JSONNode::STRING_NODE)
        Error("in GetOptionalStringValue: expected \"" + key + "\" to have a string node!");
    const JSON::StringNode * const string_node(reinterpret_cast<const JSON::StringNode * const>(value_node));
    return string_node->getValue();
}


const JSON::StringNode *CastToStringNodeOrDie(const std::string &node_name, const JSON::JSONNode *  const node) {
    if (unlikely(node->getType() != JSON::JSONNode::STRING_NODE))
        Error("in CastToStringNodeOrDie: expected \"" + node_name + "\" to be a string node!");
    return reinterpret_cast<const JSON::StringNode * const>(node);
}


void CreateCreatorFields(const JSON::JSONNode *  const creators_node, MarcRecord * const marc_record) {
    if (creators_node->getType() != JSON::JSONNode::ARRAY_NODE)
        Error("in CreateCreatorFields: expected \"creators\" to have a array node!");
    const JSON::ArrayNode * const array(reinterpret_cast<const JSON::ArrayNode * const>(creators_node));
    for (auto creator_node(array->cbegin()); creator_node != array->cend(); ++creator_node) {
        if ((*creator_node)->getType() != JSON::JSONNode::OBJECT_NODE)
            Error("in CreateCreatorFields: expected creator node to be an object node!");
        const JSON::ObjectNode * const creator_object(
            reinterpret_cast<const JSON::ObjectNode * const>(*creator_node));

        const JSON::JSONNode * const last_name_node(creator_object->getValue("lastName"));
        if (last_name_node == nullptr)
            Error("in CreateCreatorFields: creator is missing a last name!");
        const JSON::StringNode * const last_name(CastToStringNodeOrDie("lastName", last_name_node));
        std::string name(last_name->getValue());

        const JSON::JSONNode * const first_name_node(creator_object->getValue("firstName"));
        if (first_name_node != nullptr) {
            const JSON::StringNode * const first_name(CastToStringNodeOrDie("firstName", first_name_node));
            name += ", " + first_name->getValue();
        }

        const JSON::JSONNode * const creator_type(creator_object->getValue("creatorType"));
        std::string creator_role;
        if (creator_type != nullptr) {
            const JSON::StringNode * const creator_role_node(CastToStringNodeOrDie("creatorType", creator_type));
            creator_role = creator_role_node->getValue();
        }

        if (creator_node == array->cbegin()) {
            if (creator_role.empty())
                marc_record->insertSubfield("100", 'a', name);
            else
                marc_record->insertSubfields("100", { { 'a', name }, { 'e', creator_role } });
        } else { // Not the first creator!
            if (creator_role.empty())
                marc_record->insertSubfield("700", 'a', name);
            else
                marc_record->insertSubfields("700", { { 'a', name }, { 'e', creator_role } });
        }
    }
}


// If "key" is in "map", then return the mapped value, o/w return "key".
inline std::string OptionalMap(const std::string &key, const std::unordered_map<std::string, std::string> &map) {
    const auto &key_and_value(map.find(key));
    return (key_and_value == map.cend()) ? key : key_and_value->second;
}


struct Date {
    static const unsigned INVALID = 0;
    unsigned day_;
    unsigned month_;
    unsigned year_;
public:
    Date(): day_(INVALID), month_(INVALID), year_(INVALID) { }
};


Date StringToDate(const std::string &date_str) {
    Date date;

    time_t unix_time(WebUtil::ParseWebDateAndTime(date_str));
    if (unix_time != TimeUtil::BAD_TIME_T) {
        tm *tm(::gmtime(&unix_time));
        if (unlikely(tm == nullptr))
            Error("in StringToDate: gmtime(3) failed to convert a time_t! (" + date_str + ")");
        date.day_   = tm->tm_mday;
        date.month_ = tm->tm_mon;
        date.year_  = tm->tm_year;
    } else
        Warning("don't know how to convert \"" + date_str + "\" to a Date instance!");

    return date;
}


void ExtractVolumeYearIssueAndPages(const JSON::ObjectNode &object_node, MarcRecord * const new_record) {
    std::vector<std::pair<char, std::string>> subfield_codes_and_values;

    const std::string date_str(GetOptionalStringValue(object_node, "date"));
    if (not date_str.empty()) {
        const Date date(StringToDate(date_str));
        if (date.year_ != Date::INVALID)
            subfield_codes_and_values.emplace_back(std::make_pair('j', std::to_string(date.year_)));
    }

    const std::string issue(GetOptionalStringValue(object_node, "issue"));
    if (not issue.empty())
        subfield_codes_and_values.emplace_back(std::make_pair('e', issue));

    const std::string pages(GetOptionalStringValue(object_node, "pages"));
    if (not pages.empty())
        subfield_codes_and_values.emplace_back(std::make_pair('h', pages));

    const std::string volume(GetOptionalStringValue(object_node, "volume"));
    if (not volume.empty())
        subfield_codes_and_values.emplace_back(std::make_pair('d', volume));

    if (not subfield_codes_and_values.empty())
        new_record->insertSubfields("936", subfield_codes_and_values);
}


void ExtractKeywords(const JSON::JSONNode &tags_node, const std::string &issn,
                     const std::unordered_map<std::string, std::string> &ISSN_to_keyword_field_map,
                     MarcRecord * const new_record)
{
    if (unlikely(tags_node.getType() != JSON::JSONNode::ARRAY_NODE))
        Error("in ExtractKeywords: expected the tags node to be an array node!");
    const JSON::ArrayNode &tags(reinterpret_cast<const JSON::ArrayNode &>(tags_node));

    // Where to stuff the data:
    std::string marc_field("653");
    char marc_subfield('a');
    if (not issn.empty()) {
        const auto issn_and_field_tag_and_subfield_code(ISSN_to_keyword_field_map.find(issn));
        if (issn_and_field_tag_and_subfield_code != ISSN_to_keyword_field_map.end()) {
            if (unlikely(issn_and_field_tag_and_subfield_code->second.length() != 3 + 1))
                Error("in ExtractKeywords: \"" + issn_and_field_tag_and_subfield_code->second
                      + "\" is not a valid MARC tag + subfield code! (Error in \"ISSN_to_keyword_field.map\"!)");
            marc_field    = issn_and_field_tag_and_subfield_code->second.substr(0, 3);
            marc_subfield =  issn_and_field_tag_and_subfield_code->second[3];
        }
    }

    for (auto tag(tags.cbegin()); tag != tags.cend(); ++tag) {
        if ((*tag)->getType() != JSON::JSONNode::OBJECT_NODE)
            Error("in ExtractKeywords: expected tag node to be an object node but found a(n) "
                  + JSON::JSONNode::TypeToString((*tag)->getType()) + " node instead!");
        const JSON::ObjectNode * const tag_object(
                                                  reinterpret_cast<const JSON::ObjectNode * const>(*tag));
        const JSON::JSONNode * const tag_node(tag_object->getValue("tag"));
        if (tag_node == nullptr)
            Warning("in ExtractKeywords: unexpected: tag object does not contain a \"tag\" entry!");
        else if (tag_node->getType() != JSON::JSONNode::STRING_NODE)
            Error("in ExtractKeywords: unexpected: tag object's \"tag\" entry is not a string node!");
        else
            CreateSubfieldFromStringNode("tag", tag_node, marc_field, marc_subfield, new_record);
    }
}


const std::string DEFAULT_SUBFIELD_CODE("eng");


std::pair<unsigned, unsigned> GenerateMARC(
    const JSON::JSONNode * const tree, const std::unordered_map<std::string, std::string> &ISSN_to_physical_form_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_language_code_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_superior_ppn_map,
    const std::unordered_map<std::string, std::string> &language_to_language_code_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_volume_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_licence_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_keyword_field_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_SSG_map,
    std::unordered_set<std::string> * const previously_downloaded, MarcWriter * const marc_writer)
{
    if (tree->getType() != JSON::JSONNode::ARRAY_NODE)
        Error("in GenerateMARC: expected top-level JSON to be an array!");
    const JSON::ArrayNode * const top_level_array(reinterpret_cast<const JSON::ArrayNode * const>(tree));

    static RegexMatcher * const ignore_fields(RegexMatcher::RegexMatcherFactory(
        "^issue|pages|publicationTitle|volume|date|tags|libraryCatalog|itemVersion|accessDate$"));
    unsigned record_count(0), previously_downloaded_count(0);
    for (auto entry(top_level_array->cbegin()); entry != top_level_array->cend(); ++entry) {
        MarcRecord new_record;
        bool is_journal_article(false);
        std::string publication_title, parent_ppn, parent_isdn, issn;
        if ((*entry)->getType() != JSON::JSONNode::OBJECT_NODE)
            Error("in GenerateMARC: expected an object node!");
        const JSON::ObjectNode * const object_node(reinterpret_cast<const JSON::ObjectNode * const>(*entry));
        for (auto key_and_node(object_node->cbegin()); key_and_node != object_node->cend(); ++key_and_node) {
            if (ignore_fields->matched(key_and_node->first))
                continue;

            if (key_and_node->first == "itemKey") {
                const JSON::StringNode * const item_key(CastToStringNodeOrDie("itemKey", key_and_node->second));
                new_record.insertField("001", item_key->getValue());
            } else if (key_and_node->first == "language")
                new_record.insertSubfield("045", 'a',
                    OptionalMap(CastToStringNodeOrDie("language", key_and_node->second)->getValue(),
                                language_to_language_code_map));
            else if (key_and_node->first == "url")
                CreateSubfieldFromStringNode(*key_and_node, "856", 'u', &new_record);
            else if (key_and_node->first == "title")
                CreateSubfieldFromStringNode(*key_and_node, "245", 'a', &new_record);
            else if (key_and_node->first == "abstractNote")
                CreateSubfieldFromStringNode(*key_and_node, "520", 'a', &new_record, /* indicator1 = */ '3');
            else if (key_and_node->first == "date")
                CreateSubfieldFromStringNode(*key_and_node, "362", 'a', &new_record, /* indicator1 = */ '0');
            else if (key_and_node->first == "DOI") {
                if (unlikely(key_and_node->second->getType() != JSON::JSONNode::STRING_NODE))
                    Error("in GenerateMARC: expected DOI node to be a string node!");
                new_record.insertSubfield(
                    "856", 'u', "urn:doi:" + reinterpret_cast<JSON::StringNode *>(key_and_node->second)->getValue());
            } else if (key_and_node->first == "shortTitle")
                CreateSubfieldFromStringNode(*key_and_node, "246", 'a', &new_record);
            else if (key_and_node->first == "creators")
                CreateCreatorFields(key_and_node->second, &new_record);
            else if (key_and_node->first == "ISSN") {
                parent_isdn = GetValueFromStringNode(*key_and_node);
                const std::string issn_candidate(
                    CreateSubfieldFromStringNode(*key_and_node, "022", 'a', &new_record));
                if (unlikely(not MiscUtil::NormaliseISSN(issn_candidate, &issn)))
                    Error("in GenerateMARC: \"" + issn_candidate + "\" is not a valid ISSN!");

                const auto ISSN_and_physical_form(ISSN_to_physical_form_map.find(issn));
                if (ISSN_and_physical_form != ISSN_to_physical_form_map.cend()) {
                    if (ISSN_and_physical_form->second == "A")
                        new_record.insertField("007", "tu");
                    else if (ISSN_and_physical_form->second == "O")
                        new_record.insertField("007", "cr uuu---uuuuu");
                    else
                        Error("in GenerateMARC: unhandled entry in physical form map: \""
                              + ISSN_and_physical_form->second + "\"!");
                }

                const auto ISSN_and_language(ISSN_to_language_code_map.find(issn));
                if (ISSN_and_language != ISSN_to_language_code_map.cend())
                    new_record.insertSubfield("041", 'a', ISSN_and_language->second);

                const auto ISSN_and_parent_ppn(ISSN_to_superior_ppn_map.find(issn));
                if (ISSN_and_parent_ppn != ISSN_to_superior_ppn_map.cend())
                    parent_ppn = ISSN_and_parent_ppn->second;
            } else if (key_and_node->first == "itemType") {
                const std::string item_type(GetValueFromStringNode(*key_and_node));
                if (item_type == "journalArticle") {
                    is_journal_article = true;
                    publication_title = GetOptionalStringValue(*object_node, "publicationTitle");
                    ExtractVolumeYearIssueAndPages(*object_node, &new_record);
                } else if (item_type == "magazineArticle")
                    ExtractVolumeYearIssueAndPages(*object_node, &new_record);
                else
                    Warning("in GenerateMARC: unknown item type: \"" + item_type + "\"!");
            } else
                Warning("in GenerateMARC: unknown key \"" + key_and_node->first + "\" with node type "
                        + JSON::JSONNode::TypeToString(key_and_node->second->getType()) + "! ("
                        + key_and_node->second->toString() + ")");
        }

        // Handle keywords:
        const JSON::JSONNode * const tags_node(object_node->getValue("tags"));
        if (tags_node != nullptr)
            ExtractKeywords(*tags_node, issn, ISSN_to_keyword_field_map, &new_record);

        // Populate 773:
        if (is_journal_article) {
            std::vector<std::pair<char, std::string>> subfield_codes_and_values;
            if (not publication_title.empty())
                subfield_codes_and_values.emplace_back('a', publication_title);
            if (not parent_isdn.empty())
                subfield_codes_and_values.emplace_back('x', parent_isdn);
            if (not parent_ppn.empty())
                subfield_codes_and_values.emplace_back('w', "(DE-576))" + parent_ppn);
            if (not subfield_codes_and_values.empty())
                new_record.insertSubfields("773", subfield_codes_and_values);
        }

        // Make sure we always have a language code:
        if (new_record.getFieldIndex("041") == MarcRecord::FIELD_NOT_FOUND)
            new_record.insertSubfield("041", 'a', DEFAULT_SUBFIELD_CODE);

        // If we don't have a volume, check to see if we can infer one from an ISSN:
        if (not issn.empty()) {
            const auto ISSN_and_volume(ISSN_to_volume_map.find(issn));
            if (ISSN_and_volume != ISSN_to_volume_map.cend()) {
                const std::string volume(ISSN_and_volume->second);
                const size_t index(new_record.getFieldIndex("936"));
                if (index == MarcRecord::FIELD_NOT_FOUND)
                    new_record.insertSubfield("936", 'v', volume);
                else {
                    const Subfields subfields(new_record.getFieldData(index));
                    if (not subfields.hasSubfield('v'))
                        new_record.addSubfield("936", 'v', volume);
                }
            }

            const auto ISSN_and_license_code(ISSN_to_licence_map.find(issn));
            if (ISSN_and_license_code != ISSN_to_licence_map.end()) {
                if (ISSN_and_license_code->second != "l")
                    Warning("ISSN_to_licence.map contains an ISSN that has not been mapped to an \"l\" but \""
                            + ISSN_and_license_code->second + "\" instead and we don't know what to do with it!");
                else {
                    const size_t _856_index(new_record.getFieldIndex("856"));
                    if (_856_index != MarcRecord::FIELD_NOT_FOUND)
                        new_record.addSubfield("856", 'z', "Kostenfrei");
                }
            }
        }

        // Add SSG numbers:
        if (not issn.empty()) {
            const auto ISSN_and_SSGN_numbers(ISSN_to_SSG_map.find(issn));
            if (ISSN_and_SSGN_numbers != ISSN_to_SSG_map.end())
                new_record.addSubfield("084", 'a', ISSN_and_SSGN_numbers->second);
        }

        const std::string checksum(new_record.calcChecksum(/* exclude_001 = */ true));
        if (previously_downloaded->find(checksum) == previously_downloaded->cend()) {
            previously_downloaded->emplace(checksum);
            marc_writer->write(new_record);
        } else
            ++previously_downloaded_count;
        ++record_count;
    }

    return std::make_pair(record_count, previously_downloaded_count);
}


std::pair<unsigned, unsigned> Harvest(
    const std::string &zts_server_url, const std::string &harvest_url,
    const std::unordered_map<std::string, std::string> &ISSN_to_physical_form_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_language_code_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_superior_ppn_map,
    const std::unordered_map<std::string, std::string> &language_to_language_code_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_volume_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_licence_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_keyword_field_map,
    const std::unordered_map<std::string, std::string> &ISSN_to_SSG_map,
    std::unordered_set<std::string> * const previously_downloaded, MarcWriter * const marc_writer)
{
    std::string json_document, error_message;
    if (not Download(Url(zts_server_url), /* time_limit = */ 20000, harvest_url, &json_document, &error_message)) {
        std::cerr << "Download for harvest URL \"" + harvest_url + "\" failed: " + error_message << '\n';
        return std::make_pair(0, 0);
    }

    JSON::JSONNode *tree_root(nullptr);
    std::pair<unsigned, unsigned> record_count_and_previously_downloaded_count;
    try {
        JSON::Parser json_parser(json_document);
        if (not (json_parser.parse(&tree_root)))
            Error("failed to parse returned JSON: " + json_parser.getErrorMessage());

        record_count_and_previously_downloaded_count =
            GenerateMARC(tree_root, ISSN_to_physical_form_map, ISSN_to_language_code_map, ISSN_to_superior_ppn_map,
                         language_to_language_code_map, ISSN_to_volume_map, ISSN_to_licence_map,
                         ISSN_to_keyword_field_map, ISSN_to_SSG_map, previously_downloaded, marc_writer);
        delete tree_root;
    } catch (...) {
        delete tree_root;
        throw;
    }

    std::cerr << "Harvested " << record_count_and_previously_downloaded_count.first << " record(s) from "
              << harvest_url << '\n' << "of which "
              << (record_count_and_previously_downloaded_count.first
                  - record_count_and_previously_downloaded_count.second)
              << " records were new records.\n";
    return record_count_and_previously_downloaded_count;
}


void StorePreviouslyDownloadedHashes(File * const output,
                                     const std::unordered_set<std::string> &previously_downloaded)
{
    for (const auto &hash : previously_downloaded)
        output->write(TextUtil::Base64Encode(hash) + '\n');

    std::cerr << "Stored " << previously_downloaded.size() << " hashes of previously generated records.\n";
}


void LoadHarvestURLs(const bool ignore_robots_dot_txt, const std::string &zotero_crawler_config_path,
                     std::vector<std::string> * const harvest_urls)
{
    harvest_urls->clear();

    std::cerr << "Starting loading of harvest URL's.\n";

    const std::string COMMAND("/usr/local/bin/zotero_crawler"
                              + std::string(ignore_robots_dot_txt ? " --ignore-robots-dot-txt " : " ")
                              + zotero_crawler_config_path);

    std::string stdout_output;
    if (not ExecUtil::ExecSubcommandAndCaptureStdout(COMMAND, &stdout_output))
        Error("in LoadHarvestURLs: failed to execute \"" + COMMAND + "\"!");
    if (StringUtil::Split(stdout_output, '\n', harvest_urls) == 0)
        Error("in LoadHarvestURLs: no harvest URL's were read after executing \"" + COMMAND + "\"!");
    std::cerr << "Loaded " << harvest_urls->size() << " harvest URL's.\n";
}


int main(int argc, char *argv[]) {
    ::progname = argv[0];
    if (argc < 4 or argc > 7)
        Usage();

    bool ignore_robots_dot_txt(false);
    if (std::strcmp(argv[1], "--ignore-robots-dot-txt") == 0) {
        ignore_robots_dot_txt = true;
        --argc, ++argv;
    }

    std::string zotero_crawler_config_path;
    const std::string CONFIG_FLAG_PREFIX("--zotero-crawler-config-file=");
    if (StringUtil::StartsWith(argv[1], CONFIG_FLAG_PREFIX)) {
        zotero_crawler_config_path = argv[1] + CONFIG_FLAG_PREFIX.length();
        --argc, ++argv;
    } else
        zotero_crawler_config_path = DEFAULT_ZOTOERO_CRAWLER_CONFIG_PATH;

    std::string progress_filename;
    const std::string PROGRESS_FILE_FLAG_PREFIX("--progress-file=");
    if (StringUtil::StartsWith(argv[1], PROGRESS_FILE_FLAG_PREFIX)) {
        progress_filename = argv[1] + PROGRESS_FILE_FLAG_PREFIX.length();
        --argc, ++argv;
    }

    if (argc != 4)
        Usage();

    const std::string ZTS_SERVER_URL(argv[1]);
    std::string map_directory_path(argv[2]);
    if (not StringUtil::EndsWith(map_directory_path, '/'))
        map_directory_path += '/';

    try {
        std::unordered_map<std::string, std::string> ISSN_to_physical_form_map;
        LoadMapFile(map_directory_path + "ISSN_to_physical_form.map", &ISSN_to_physical_form_map);

        std::unordered_map<std::string, std::string> ISSN_to_language_code_map;
        LoadMapFile(map_directory_path + "ISSN_to_language_code.map", &ISSN_to_language_code_map);

        std::unordered_map<std::string, std::string> ISSN_to_superior_ppn_map;
        LoadMapFile(map_directory_path + "ISSN_to_superior_ppn.map", &ISSN_to_superior_ppn_map);

        std::unordered_map<std::string, std::string> language_to_language_code_map;
        LoadMapFile(map_directory_path + "language_to_language_code.map", &language_to_language_code_map);

        std::unordered_map<std::string, std::string> ISSN_to_volume_map;
        LoadMapFile(map_directory_path + "ISSN_to_volume.map", &ISSN_to_volume_map);

        std::unordered_map<std::string, std::string> ISSN_to_licence_map;
        LoadMapFile(map_directory_path + "ISSN_to_licence.map", &ISSN_to_licence_map);

        std::unordered_map<std::string, std::string> ISSN_to_keyword_field_map;
        LoadMapFile(map_directory_path + "ISSN_to_keyword_field.map", &ISSN_to_keyword_field_map);

        std::unordered_map<std::string, std::string> ISSN_to_SSG_map;
        LoadMapFile(map_directory_path + "ISSN_to_SSG.map", &ISSN_to_SSG_map);

        const RegexMatcher * const supported_urls_regex(LoadSupportedURLsRegex(map_directory_path));
        (void)supported_urls_regex;

        std::unordered_set<std::string> previously_downloaded;
        const std::string PREVIOUSLY_DOWNLOADED_HASHES_PATH(map_directory_path + "previously_downloaded.hashes");
        if (FileUtil::Exists(PREVIOUSLY_DOWNLOADED_HASHES_PATH)) {
            std::unique_ptr<File> previously_downloaded_input(
                FileUtil::OpenInputFileOrDie(PREVIOUSLY_DOWNLOADED_HASHES_PATH));
            LoadPreviouslyDownloadedHashes(previously_downloaded_input.get(), &previously_downloaded);
        }

        std::unique_ptr<MarcWriter> marc_writer(MarcWriter::Factory(argv[3]));
        unsigned total_record_count(0), total_previously_downloaded_count(0);

        std::unique_ptr<File> progress_file;
        if (not progress_filename.empty())
            progress_file = FileUtil::OpenOutputFileOrDie(progress_filename);

        std::vector<std::string> harvest_urls;
        LoadHarvestURLs(ignore_robots_dot_txt, zotero_crawler_config_path, &harvest_urls);
        unsigned i(0);
        for (const auto &harvest_url : harvest_urls) {
            const auto record_count_and_previously_downloaded_count(
                Harvest(ZTS_SERVER_URL, harvest_url, ISSN_to_physical_form_map, ISSN_to_language_code_map,
                        ISSN_to_superior_ppn_map, language_to_language_code_map, ISSN_to_volume_map,
                        ISSN_to_licence_map, ISSN_to_keyword_field_map, ISSN_to_SSG_map, &previously_downloaded,
                        marc_writer.get()));
            total_record_count                += record_count_and_previously_downloaded_count.first;
            total_previously_downloaded_count += record_count_and_previously_downloaded_count.second;
            if (progress_file != nullptr) {
                progress_file->rewind();
                ++i;
                if (unlikely(not progress_file->write(
                        StringUtil::Format("%06f", static_cast<double>(i) / harvest_urls.size()))))
                    Error("failed to write progress to \"" + progress_file->getPath());
            }
        }

        std::cout << "Harvested a total of " << total_record_count << " records of which "
                  << total_previously_downloaded_count << " were already previously downloaded.\n";

        std::unique_ptr<File> previously_downloaded_output(
            FileUtil::OpenOutputFileOrDie(map_directory_path + "previously_downloaded.hashes"));
        StorePreviouslyDownloadedHashes(previously_downloaded_output.get(), previously_downloaded);
    } catch (const std::exception &x) {
        Error("caught exception: " + std::string(x.what()));
    }
}