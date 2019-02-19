/** \brief Utility for merging print and online editions into single records.
 *  \author Dr. Johannes Ruscheinski (johannes.ruscheinski@uni-tuebingen.de)
 *
 *  \copyright 2018,2019 Universitätsbibliothek Tübingen.  All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdlib>
#include "DbConnection.h"
#include "DbResultSet.h"
#include "FileUtil.h"
#include "MARC.h"
#include "StringUtil.h"
#include "TextUtil.h"
#include "util.h"
#include "VuFind.h"


namespace {


[[noreturn]] void Usage() {
    std::cerr << "Usage: " << ::progname
              << " [--min-log-level=min_log_level] [--debug] marc_input marc_output missing_ppn_partners_list\n"
              << "       missing_ppn_partners_list will be generated by this program and will contain the PPN's\n"
              << "       of superior works with cross links between print and online edition with one of\n"
              << "       the partners missing.  N.B. the input MARC file *must* be in the MARC-21 format!\n\n";
    std::exit(EXIT_FAILURE);
}


const std::set<MARC::Tag> UPLINK_TAGS{ "800", "810", "830", "773", "776" };


std::string ExtractUplinkPPN(const MARC::Record::Field &field) {
    const MARC::Subfields subfields(field.getSubfields());
    auto subfield_w(std::find_if(subfields.begin(), subfields.end(),
                                 [](const MARC::Subfield &subfield) -> bool { return subfield.code_ == 'w'; }));
    if (subfield_w == subfields.end())
        return "";

    if (not StringUtil::StartsWith(subfield_w->value_, "(DE-576)"))
        return "";
    return subfield_w->value_.substr(__builtin_strlen("(DE-576)"));
}


template<typename ValueType>void SerializeMap(const std::string &output_filename, const std::unordered_map<std::string, ValueType> &map) {
    const auto map_file(FileUtil::OpenOutputFileOrDie(output_filename));
    for (const auto &key_and_value : map)
        *map_file << key_and_value.first << " -> " << key_and_value.second << '\n';
}


void SerializeMultimap(const std::string &output_filename, const std::unordered_multimap<std::string, std::string> &multimap) {
    const auto map_file(FileUtil::OpenOutputFileOrDie(output_filename));
    if (multimap.empty())
        return;

    auto key_and_value(multimap.cbegin());
    std::string last_key(key_and_value->first);
    *map_file << key_and_value->first << " -> " << key_and_value->second;
    for (/* Intentionally empty! */; key_and_value != multimap.cend(); ++key_and_value) {
        if (key_and_value->first == last_key)
            *map_file << ',' << key_and_value->second;
        else {
            last_key = key_and_value->first;
            *map_file << '\n' << key_and_value->first << " -> " << key_and_value->second;
        }
    }
    *map_file << '\n';
}


void CollectRecordOffsetsAndCrosslinks(const bool debug,
    MARC::Reader * const marc_reader, std::unordered_map<std::string, off_t> * const ppn_to_offset_map,
    std::unordered_map<std::string, std::string> * const ppn_to_canonical_ppn_map,
    std::unordered_multimap<std::string, std::string> * const canonical_ppn_to_ppn_map)
{
    off_t last_offset(0);
    unsigned record_count(0);
    while (const auto record = marc_reader->read()) {
        ++record_count;

        if (unlikely(ppn_to_offset_map->find(record.getControlNumber()) != ppn_to_offset_map->end()))
            LOG_ERROR("duplicate PPN \"" + record.getControlNumber() + "\" in input file \"" + marc_reader->getPath() + "\"!");

        (*ppn_to_offset_map)[record.getControlNumber()] = last_offset;

        last_offset = marc_reader->tell();

        // We only want to merge serials!
        if (not record.isSerial())
            continue;

        // In the following lines, we get all cross referenced PPN's and check the maps for their references as well.
        // We then determine the new superior PPN for all cross refences and overwrite all existing mapping entries.
        auto cross_link_ppns(MARC::ExtractCrossReferencePPNs(record));
        if (not cross_link_ppns.empty()) {

            // check maps for additional referenced ppns
            for (const auto &ppn : cross_link_ppns) {
                const auto it(ppn_to_canonical_ppn_map->find(ppn));
                if (it != ppn_to_canonical_ppn_map->end())
                    cross_link_ppns.emplace(it->second);

                auto canonical_ppn_and_ppn_range(canonical_ppn_to_ppn_map->equal_range(ppn));
                for (auto it2 = canonical_ppn_and_ppn_range.first; it2 != canonical_ppn_and_ppn_range.second; ++it2)
                    cross_link_ppns.emplace(it2->second);
            }
            cross_link_ppns.emplace(record.getControlNumber());

            // The max PPN, will be the winner for merging, IOW, it will be the PPN of the merged record.
            const std::string max_ppn(*std::max_element(cross_link_ppns.begin(), cross_link_ppns.end(),
                                                        [](std::string a, std::string b) -> bool { return a < b; }));

            // remove old references
            for (const auto &ppn : cross_link_ppns) {
                ppn_to_canonical_ppn_map->erase(ppn);
                canonical_ppn_to_ppn_map->erase(ppn);
            }

            // add new/updated references
            for (const auto &ppn : cross_link_ppns) {
                if (ppn != max_ppn) {
                    ppn_to_canonical_ppn_map->emplace(ppn, max_ppn);
                    canonical_ppn_to_ppn_map->emplace(max_ppn, ppn);
                }
            }
        }
    }

    if (debug) {
        std::string map_filename("ppn_to_canonical_ppn.map");
        SerializeMap(map_filename, *ppn_to_canonical_ppn_map);
        std::cerr << "Wrote the mapping from non-canonical PPN's to canonical PPN's to \"" + map_filename + "\"!";

        map_filename = "canonical_ppn_to_ppn.map";
        SerializeMultimap(map_filename, *canonical_ppn_to_ppn_map);
        std::cerr << "Wrote the mapping from canonical PPN's to non-canonical PPN's to \"" + map_filename + "\"!";

        map_filename = "ppn_to_offset.map";
        SerializeMap(map_filename, *ppn_to_offset_map);
        std::cerr << "Wrote the mapping from canonical PPN's to non-canonical PPN's to \"" + map_filename + "\"!";
    }

    LOG_INFO("Found " + std::to_string(record_count) + " record(s).");
    LOG_INFO("Found " + std::to_string(ppn_to_canonical_ppn_map->size()) + " cross link(s).");
}


void EliminateDanglingOrUnreferencedCrossLinks(const bool debug, const std::unordered_map<std::string, off_t> &ppn_to_offset_map,
                                               std::unordered_map<std::string, std::string> * const ppn_to_canonical_ppn_map,
                                               std::unordered_multimap<std::string, std::string> * const canonical_ppn_to_ppn_map)
{
    unsigned dropped_count(0);
    auto canonical_ppn_and_ppn(canonical_ppn_to_ppn_map->begin());
    std::set<std::string> group_ppns;
    while (canonical_ppn_and_ppn != canonical_ppn_to_ppn_map->end()) {
        const std::string canonical_ppn(canonical_ppn_and_ppn->first);
        group_ppns.emplace(canonical_ppn_and_ppn->second);
        const auto next_canonical_ppn_and_ppn(std::next(canonical_ppn_and_ppn, 1));
        bool drop_group(false);

        if (next_canonical_ppn_and_ppn == canonical_ppn_to_ppn_map->end()
            or next_canonical_ppn_and_ppn->first != canonical_ppn)
        {
            // Decide to drop group either if PPN for merging is not a superior PPN or doesn't exist...
            drop_group = ppn_to_offset_map.find(canonical_ppn) == ppn_to_offset_map.end();

            // ... or at least one of the PPN's doesn't exist
            if (not drop_group) {
                for (const auto &ppn : group_ppns) {
                    if (ppn_to_offset_map.find(ppn) == ppn_to_offset_map.end()) {
                        LOG_INFO("Don't merge group around PPN " + ppn + " because the PPN is missing in our data! All PPNs in group: "
                                 + StringUtil::Join(group_ppns, ','));
                        drop_group = true;
                        break;
                    }
                }
            }

            // Do drop
            if (drop_group) {
                for (const auto &ppn : group_ppns)
                    ppn_to_canonical_ppn_map->erase(ppn);

                dropped_count += group_ppns.size() + 1;
                while (canonical_ppn_to_ppn_map->find(canonical_ppn) != canonical_ppn_to_ppn_map->end())
                    canonical_ppn_and_ppn = canonical_ppn_to_ppn_map->erase(canonical_ppn_to_ppn_map->find(canonical_ppn));

            }

            group_ppns.clear();
        }

        if (not drop_group)
            ++canonical_ppn_and_ppn;
    }

    if (debug) {
        std::string map_filename("ppn_to_canonical_ppn2.map");
        SerializeMap(map_filename, *ppn_to_canonical_ppn_map);
        std::cerr << "Wrote the mapping from non-canonical PPN's to canonical PPN's to \"" + map_filename + "\"!";

        map_filename = "canonical_ppn_to_ppn2.map";
        SerializeMultimap(map_filename, *canonical_ppn_to_ppn_map);
        std::cerr << "Wrote the mapping from canonical PPN's to non-canonical PPN's to \"" + map_filename + "\"!";
    }

    LOG_INFO("Dropped " + std::to_string(dropped_count) + " cross link(s) because at least one end was not a superior work or is missing.");
}


// Make inferior works point to the new merged superior parent found in "ppn_to_canonical_ppn_map".  Links referencing a key in
// "ppn_to_canonical_ppn_map" will be patched with the corresponding value.
// only 1 uplink of the same tag type will be kept.
unsigned PatchUplinks(MARC::Record * const record, const std::unordered_map<std::string, std::string> &ppn_to_canonical_ppn_map) {
    unsigned patched_uplinks(0);

    std::vector<size_t> uplink_indices_for_deletion;
    std::set<std::string> uplink_tags_done;
    for (auto field(record->begin()); field != record->end(); ++field) {
        const std::string field_tag(field->getTag().toString());
        if (UPLINK_TAGS.find(field_tag) != UPLINK_TAGS.cend()) {
            const std::string uplink_ppn(ExtractUplinkPPN(*field));
            if (uplink_ppn.empty())
                continue;

            if (uplink_tags_done.find(field_tag) != uplink_tags_done.end()) {
                uplink_indices_for_deletion.emplace_back(field - record->begin());
                continue;
            }

            const auto ppn_and_ppn(ppn_to_canonical_ppn_map.find(uplink_ppn));
            if (ppn_and_ppn == ppn_to_canonical_ppn_map.end())
                continue;

            // If we made it here, we need to replace the uplink PPN:
            field->insertOrReplaceSubfield('w', "(DE-576)" + ppn_and_ppn->second);
            uplink_tags_done.emplace(field_tag);
            ++patched_uplinks;
        }
    }

    record->deleteFields(uplink_indices_for_deletion);
    return patched_uplinks;
}


// The strategy we employ here is that we just pick "contents1" unless we have an identical subfield structure.
MARC::Subfields MergeFieldContents(const MARC::Subfields &subfields1, const bool record1_is_electronic,
                                   const MARC::Subfields &subfields2, const bool record2_is_electronic)
{
    std::string subfield_codes1;
    for (const auto &subfield : subfields1)
        subfield_codes1 += subfield.code_;

    std::string subfield_codes2;
    for (const auto &subfield : subfields2)
        subfield_codes2 += subfield.code_;

    if (subfield_codes1 != subfield_codes2) // We are up the creek!
        return subfields1;

    MARC::Subfields merged_subfields;
    for (auto subfield1(subfields1.begin()), subfield2(subfields2.begin()); subfield1 != subfields1.end(); ++subfield1, ++subfield2) {
        if (subfield1->value_ == subfield2->value_)
            merged_subfields.addSubfield(subfield1->code_, subfield1->value_);
        else {
            std::string merged_value(subfield1->value_);
            merged_value += " (";
            merged_value += record1_is_electronic ? "electronic" : "print";
            merged_value += "); ";
            merged_value += subfield2->value_;
            merged_value += " (";
            merged_value += record2_is_electronic ? "electronic" : "print";
            merged_value += ')';
            merged_subfields.addSubfield(subfield1->code_, merged_value);
        }
    }

    return merged_subfields;
}


MARC::Record::Field MergeControlFields(const MARC::Tag &tag, const std::string &field_contents1, const std::string &field_contents2) {
    std::string merged_contents;

    if (tag == "005") // Date and Time of Latest Transaction
        merged_contents = std::max(field_contents1, field_contents2);
    else
        merged_contents = field_contents1;

    return MARC::Record::Field(tag, merged_contents);
}


std::string CanoniseText(const std::string &s) {
    std::vector<uint32_t> utf32_chars;
    if (unlikely(not TextUtil::UTF8ToUTF32(s, &utf32_chars)))
        return s;

    static const uint32_t SPACE(0x20u);

    std::vector<uint32_t> clean_utf32_chars;

    // Remove leading whitespace and replace multiple occurrences of whitespace characters with a single space and
    // convert any other characters to lowercase:
    bool whitespace_seen(true);
    for (const uint32_t ch : utf32_chars) {
        if (TextUtil::IsWhitespace(ch)) {
            if (not whitespace_seen) {
                whitespace_seen = true;
                clean_utf32_chars.emplace_back(SPACE);
            }
        } else {
            clean_utf32_chars.emplace_back(TextUtil::UTF32ToLower(ch));
            whitespace_seen = false;
        }
    }

    // Remove any trailing commas and whitespace:
    static const uint32_t COMMA(0x2Cu);
    size_t trim_count(0);
    for (auto ch_iter(clean_utf32_chars.rbegin()); ch_iter != clean_utf32_chars.rend(); ++ch_iter) {
        if (TextUtil::IsWhitespace(*ch_iter) or *ch_iter == COMMA)
            ++trim_count;
    }
    clean_utf32_chars.resize(clean_utf32_chars.size() - trim_count);

    std::string clean_s;
    for (const uint32_t utf32_ch : clean_utf32_chars) {
        if (unlikely(TextUtil::IsSomeKindOfDash(utf32_ch)))
            clean_s += '-'; // ordinary minus
        else
            clean_s += TextUtil::UTF32ToUTF8(utf32_ch);
    }

    return clean_s;
}


// Returns true if the contents of the leading subfields with subfield codes "subfield_codes" in field1 and field2 are
// identical, else returns false. Please note that the code specified in "subfield_codes" must exist.
bool SubfieldPrefixIsIdentical(const MARC::Record::Field &field1, const MARC::Record::Field &field2, const std::vector<char> &subfield_codes)
{
    const MARC::Subfields subfields1(field1.getSubfields());
    const auto subfield1(subfields1.begin());

    const MARC::Subfields subfields2(field2.getSubfields());
    const auto subfield2(subfields2.begin());

    for (const char subfield_code : subfield_codes) {
        if (subfield1 == subfields1.end() or subfield2 == subfields2.end())
            return false;
        if (subfield1->code_ != subfield_code or subfield2->code_ != subfield_code)
            return false;
        if (CanoniseText(subfield1->value_) != CanoniseText(subfield2->value_))
            return false;
    }

    return true;
}


void CollectZWI_PPNs(const MARC::Record &record, std::set<std::string> * const zwi_ppns) {
    const auto zwi_field(record.getFirstField("ZWI"));
    if (zwi_field != record.end()) {
        for (const auto &subfield : zwi_field->getSubfields()) {
            if (subfield.code_ == 'b')
                zwi_ppns->emplace(subfield.value_);
        }
    }
}


// Add or update a ZWI field in "*record" w/ all ZWI PPN's from "*record" and "record2".
void AddMergedPPNs(MARC::Record * const record, const MARC::Record &record2, const std::string &new_merged_ppn) {
    std::set<std::string> merged_ppns{ new_merged_ppn };
    CollectZWI_PPNs(*record, &merged_ppns);
    CollectZWI_PPNs(record2, &merged_ppns);

    MARC::Subfields zwi_subfields;
    zwi_subfields.addSubfield('a', "1");
    for (const auto &merged_ppn : merged_ppns)
        zwi_subfields.addSubfield('b', merged_ppn);

    record->replaceField("ZWI", zwi_subfields);
}


MARC::Record MergeRecordPair(MARC::Record &record1, MARC::Record &record2) {
    record1.reTag("260", "264");
    record2.reTag("260", "264");

    MARC::Record merged_record(record1.getLeader());

    const auto record1_end_or_lok_start(record1.getFirstField("LOK"));
    record1.sortFields(record1.begin(), record1_end_or_lok_start);
    auto record1_field(record1.begin());

    const auto record2_end_or_lok_start(record2.getFirstField("LOK"));
    record2.sortFields(record2.begin(), record2_end_or_lok_start);
    auto record2_field(record2.begin());

    while (record1_field != record1_end_or_lok_start and record2_field != record2_end_or_lok_start) {
        // Avoid duplicate fields:
        if (not merged_record.empty()) {
            if (merged_record.back() == *record1_field) {
                ++record1_field;
                continue;
            } else if (merged_record.back() == *record2_field) {
                ++record2_field;
                continue;
            }
        }

        if (record1_field->getTag() == record2_field->getTag() and not MARC::IsRepeatableField(record1_field->getTag())) {
            if (record1_field->isControlField())
                merged_record.appendField(MergeControlFields(record1_field->getTag(), record1_field->getContents(),
                                                             record2_field->getContents()));
            else
                merged_record.appendField(record1_field->getTag(),
                                          MergeFieldContents(record1_field->getSubfields(), record1.isElectronicResource(),
                                                             record2_field->getSubfields(), record2.isElectronicResource()),
                                          record1_field->getIndicator1(), record1_field->getIndicator2());
            ++record1_field, ++record2_field;
        } else if (record1_field->getTag() == record2_field->getTag() and record1_field->getTag() == "022") {
            // Special handling for the ISSN's.

            MARC::Record::Field record1_022_field(*record1_field);
            if (record1.isElectronicResource())
                record1_022_field.insertOrReplaceSubfield('2', "electronic");
            else
                record1_022_field.insertOrReplaceSubfield('2', "print");
            merged_record.appendField(record1_022_field);

            MARC::Record::Field record2_022_field(*record2_field);
            if (record2.isElectronicResource())
                record2_022_field.insertOrReplaceSubfield('2', "electronic");
            else
                record2_022_field.insertOrReplaceSubfield('2', "print");
            merged_record.appendField(record2_022_field);

            ++record1_field, ++record2_field;
        } else if (record1_field->getTag() == "264" and record2_field->getTag() == "264"
                   and SubfieldPrefixIsIdentical(*record1_field, *record2_field, {'a', 'b'}))
        {
            std::string merged_c_subfield;
            const MARC::Subfields subfields1(record1_field->getSubfields());
            const std::string subfield_c1(subfields1.getFirstSubfieldWithCode('c'));
            const MARC::Subfields subfields2(record2_field->getSubfields());
            const std::string subfield_c2(subfields2.getFirstSubfieldWithCode('c'));
            if (subfield_c1 == subfield_c2)
                merged_c_subfield = subfield_c1;
            else {
                if (not subfield_c1.empty())
                    merged_c_subfield = subfield_c1 + " (" + (record1.isElectronicResource() ? "electronic" : "print") + ")";
                if (not subfield_c2.empty()) {
                    if (not merged_c_subfield.empty())
                        merged_c_subfield += "; ";
                    merged_c_subfield = subfield_c2 + " (" + (record2.isElectronicResource() ? "electronic" : "print") + ")";
                }
            }

            if (merged_c_subfield.empty())
                merged_record.appendField(*record1_field);
            else {
                MARC::Record::Field merged_field(*record1_field);
                merged_field.insertOrReplaceSubfield('c', merged_c_subfield);
                merged_record.appendField(merged_field);
            }
            ++record1_field, ++record2_field;
        } else if (record1_field->getTag() == "936" and record2_field->getTag() == "936") {
            const auto &contents1(record1_field->getContents());
            const auto &contents2(record2_field->getContents());
            if (CanoniseText(contents1) == CanoniseText(contents2))
                merged_record.appendField(*record1_field);
            else if (contents1.find('?') != std::string::npos)
                merged_record.appendField(*record2_field);
            else if (contents2.find('?') != std::string::npos)
                merged_record.appendField(*record1_field);
            else {
                LOG_WARNING("don't know how to merge 936 fields! (field1=\"" + contents1 + "\",field2=\"" + contents2
                            + "\"), arbitrarily keeping field1");
                merged_record.appendField(*record1_field);
            }
            ++record1_field;
            ++record2_field;
        } else if (*record1_field < *record2_field) {
            merged_record.appendField(*record1_field);
            ++record1_field;
        } else if (*record2_field < *record1_field) {
            merged_record.appendField(*record2_field);
            ++record2_field;
        } else { // Both fields are identical => just take any one of them.
            merged_record.appendField(*record1_field);
            ++record1_field, ++record2_field;
        }
    }

    // Append local data, if we have any:
    if (record1_end_or_lok_start != record1.end()) {
        for (record1_field = record1_end_or_lok_start; record1_field != record1.end(); ++record1_field)
            merged_record.appendField(*record1_field);
    } else if (record2_end_or_lok_start != record2.end()) {
        for (record2_field = record2_end_or_lok_start; record2_field != record2.end(); ++record2_field)
            merged_record.appendField(*record2_field);
    }

    // Mark the record as being both "print" as well as "electronic" and store the PPN's of the dropped records:
    AddMergedPPNs(&merged_record, record2, std::min(record1.getControlNumber(), record2.getControlNumber()));
    LOG_INFO("Merged records with PPN's " + record1.getControlNumber() + " and " + record2.getControlNumber() + ".");

    return merged_record;
}


MARC::Record ReadRecordFromOffsetOrDie(MARC::Reader * const marc_reader, const off_t offset) {
    const auto saved_offset(marc_reader->tell());
    if (unlikely(not marc_reader->seek(offset)))
        LOG_ERROR("can't seek to offset " + std::to_string(offset) + "!");
    MARC::Record record(marc_reader->read());
    if (unlikely(not record))
        LOG_ERROR("failed to read a record from offset " + std::to_string(offset) + "!");

    if (unlikely(not marc_reader->seek(saved_offset)))
        LOG_ERROR("failed to seek to previous position " + std::to_string(saved_offset) + "!");

    return record;
}


// Replaces 246$i "Nebentitel:" w/ "Abweichender Titel" (RDA).
MARC::Record &Patch246i(MARC::Record * const record) {
    for (auto &_246_field : record->getTagRange("246")) {
        MARC::Subfields _246_subfields(_246_field.getSubfields());
        if (_246_subfields.replaceAllSubfields('i', "Nebentitel:", "Abweichender Titel"))
            _246_field.setSubfields(_246_subfields);
    }

    return *record;
}


void DeleteCrossLinkFields(MARC::Record * const record) {
    std::vector<size_t> field_indices_for_deletion;
    for (auto field(record->begin()); field != record->end(); ++field) {
        std::string unused;
        if (MARC::IsCrossLinkField(*field, &unused))
            field_indices_for_deletion.emplace_back(field - record->begin());
    }
    record->deleteFields(field_indices_for_deletion);
}


// Merges the records in ppn_to_canonical_ppn_map in such a way that for each entry, "second" will be merged into "first".
// "second" will then be collected in "skip_ppns" for a future copy phase where it will be dropped.  Uplinks that referenced
// "second" will be replaced with "first".
void MergeRecordsAndPatchUplinks(const bool /*debug*/, MARC::Reader * const marc_reader, MARC::Writer * const marc_writer,
                                 const std::unordered_map<std::string, off_t> &ppn_to_offset_map,
                                 const std::unordered_map<std::string, std::string> &ppn_to_canonical_ppn_map,
                                 const std::unordered_multimap<std::string, std::string> &canonical_ppn_to_ppn_map)
{
    unsigned merged_count(0), patched_uplink_count(0);
    std::unordered_set<std::string> unprocessed_ppns;
    for (const auto &ppn_and_ppn : canonical_ppn_to_ppn_map)
        unprocessed_ppns.emplace(ppn_and_ppn.second);

    while (MARC::Record record = marc_reader->read()) {
        if (ppn_to_canonical_ppn_map.find(record.getControlNumber()) != ppn_to_canonical_ppn_map.cend())
            continue; // This record will be merged into the one w/ the canonical PPN.

        auto canonical_ppn_and_ppn(canonical_ppn_to_ppn_map.find(record.getControlNumber()));
        if (canonical_ppn_and_ppn != canonical_ppn_to_ppn_map.cend()) {
            for (/* Intentionally empty! */;
                 canonical_ppn_and_ppn != canonical_ppn_to_ppn_map.cend() and canonical_ppn_and_ppn->first == record.getControlNumber();
                 ++canonical_ppn_and_ppn)
            {
                const auto record2_ppn_and_offset(ppn_to_offset_map.find(canonical_ppn_and_ppn->second));
                if (unlikely(record2_ppn_and_offset == ppn_to_offset_map.cend()))
                    LOG_ERROR("this should *never* happen! missing PPN in ppn_to_offset_map: " + canonical_ppn_and_ppn->second);
                MARC::Record record2(ReadRecordFromOffsetOrDie(marc_reader, record2_ppn_and_offset->second));
                record = MergeRecordPair(Patch246i(&record), Patch246i(&record2));
                ++merged_count;
                unprocessed_ppns.erase(canonical_ppn_and_ppn->second);
            }
            DeleteCrossLinkFields(&record);
        }

        patched_uplink_count += PatchUplinks(&record, ppn_to_canonical_ppn_map);

        marc_writer->write(record);
    }

    if (unlikely(merged_count != canonical_ppn_to_ppn_map.size())) {
        LOG_ERROR("sanity check failed! (merged_count=" + std::to_string(merged_count) + ", canonical_ppn_to_ppn_map.size()="
                  + std::to_string(canonical_ppn_to_ppn_map.size()) + ". Missing PPNs: " + StringUtil::Join(unprocessed_ppns, ", "));
    }

    LOG_INFO("Patched uplinks of " + std::to_string(patched_uplink_count) + " MARC record(s).");
}


// Here we update subscriptions.  There are 3 possible cases for each user and mapped PPN:
// 1. The trivial case where no subscriptions exist for a mapped PPN.
// 2. A subscription only exists for the mapped PPN.
//    In this case we only have to swap the PPN for the subscription.
// 3. Subscriptions exist for both, electronic and print PPNs.
//    Here we have to delete the subscription for the mapped PPN and ensure that the max_last_modification_time of the
//    remaining subscription is the minimum of the two previously existing subscriptions.
void PatchSerialSubscriptions(DbConnection * connection, const std::unordered_map<std::string, std::string> &ppn_to_canonical_ppn_map) {
    for (const auto &ppn_and_ppn : ppn_to_canonical_ppn_map) {
        connection->queryOrDie("SELECT user_id,max_last_modification_time FROM ixtheo_journal_subscriptions WHERE "
                               "journal_control_number_or_bundle_name='" + ppn_and_ppn.first + "'");
        DbResultSet ppn_first_result_set(connection->getLastResultSet());
        while (const DbRow ppn_first_row = ppn_first_result_set.getNextRow()) {
            const std::string user_id(ppn_first_row["user_id"]);
            connection->queryOrDie("SELECT max_last_modification_time FROM ixtheo_journal_subscriptions "
                                   "WHERE user_id='" + user_id + "' AND journal_control_number_or_bundle_name='" + ppn_and_ppn.second + "'");
            DbResultSet ppn_second_result_set(connection->getLastResultSet());
            if (ppn_second_result_set.empty()) {
                connection->queryOrDie("UPDATE ixtheo_journal_subscriptions SET journal_control_number_or_bundle_name='"
                                       + ppn_and_ppn.second + "' WHERE user_id='" + user_id + "' AND journal_control_number_or_bundle_name='"
                                       + ppn_and_ppn.first + "'");
                continue;
            }

            //
            // If we get here we have subscriptions for both, the electronic and the print serial and need to merge them.
            //

            const DbRow ppn_second_row(ppn_second_result_set.getNextRow());
            const std::string min_max_last_modification_time(
                (ppn_second_row["max_last_modification_time"] < ppn_first_row["max_last_modification_time"])
                    ? ppn_second_row["max_last_modification_time"]
                    : ppn_first_row["max_last_modification_time"]);
            connection->queryOrDie("DELETE FROM ixtheo_journal_subscriptions WHERE journal_control_number_or_bundle_name='"
                                   + ppn_and_ppn.first + "' and user_id='" + user_id + "'");
            if (ppn_first_row["max_last_modification_time"] > min_max_last_modification_time)
                connection->queryOrDie("UPDATE ixtheo_journal_subscriptions SET max_last_modification_time='"
                                       + min_max_last_modification_time + "' WHERE journal_control_number_or_bundle_name='"
                                       + ppn_and_ppn.second + "' and user_id='" + user_id + "'");
        }
    }
}


void PatchPDASubscriptions(DbConnection * connection, const std::unordered_map<std::string, std::string> &ppn_to_canonical_ppn_map) {
    for (const auto &ppn_and_ppn : ppn_to_canonical_ppn_map) {
        connection->queryOrDie("SELECT id FROM ixtheo_pda_subscriptions WHERE book_ppn='" + ppn_and_ppn.first + "'");
        DbResultSet result_set(connection->getLastResultSet());
        while (const DbRow row = result_set.getNextRow())
            connection->queryOrDie("UPDATE ixtheo_pda_subscriptions SET book_ppn='" + ppn_and_ppn.first + "' WHERE id='"
                                   + row["id"] + "' AND book_ppn='" + ppn_and_ppn.second + "'");
    }
}


void PatchResourceTable(DbConnection * connection, const std::unordered_map<std::string, std::string> &ppn_to_canonical_ppn_map) {
    for (const auto &ppn_and_ppn : ppn_to_canonical_ppn_map) {
        connection->queryOrDie("SELECT id FROM resource WHERE record_id='" + ppn_and_ppn.first + "'");
        DbResultSet result_set(connection->getLastResultSet());
        while (const DbRow row = result_set.getNextRow())
            connection->queryOrDie("UPDATE resource SET record_id='" + ppn_and_ppn.second + "' WHERE id=" + row["id"]);
    }
}


} // unnamed namespace


int Main(int argc, char *argv[]) {
    if (argc < 4)
        Usage();

    bool debug(false);
    if (std::strcmp(argv[1], "--debug") == 0) {
        debug = true;
        --argc, ++argv;
        if (argc < 4)
            Usage();
    }

    if (argc != 4)
        Usage();

    std::unique_ptr<MARC::Reader> marc_reader(MARC::Reader::Factory(argv[1], MARC::FileType::BINARY));
    std::unique_ptr<MARC::Writer> marc_writer(MARC::Writer::Factory(argv[2]));
    std::unique_ptr<File> missing_partners(FileUtil::OpenOutputFileOrDie(argv[3]));

    std::unordered_map<std::string, off_t> ppn_to_offset_map;
    std::unordered_map<std::string, std::string> ppn_to_canonical_ppn_map;
    std::unordered_multimap<std::string, std::string> canonical_ppn_to_ppn_map;
    CollectRecordOffsetsAndCrosslinks(debug, marc_reader.get(), &ppn_to_offset_map,
                                      &ppn_to_canonical_ppn_map, &canonical_ppn_to_ppn_map);

    EliminateDanglingOrUnreferencedCrossLinks(debug, ppn_to_offset_map, &ppn_to_canonical_ppn_map, &canonical_ppn_to_ppn_map);

    marc_reader->rewind();
    MergeRecordsAndPatchUplinks(debug, marc_reader.get(), marc_writer.get(), ppn_to_offset_map, ppn_to_canonical_ppn_map,
                                canonical_ppn_to_ppn_map);

    if (not debug) {
        std::string mysql_url;
        VuFind::GetMysqlURL(&mysql_url);
        DbConnection db_connection(mysql_url);
        PatchSerialSubscriptions(&db_connection, ppn_to_canonical_ppn_map);
        PatchPDASubscriptions(&db_connection, ppn_to_canonical_ppn_map);
        PatchResourceTable(&db_connection, ppn_to_canonical_ppn_map);
    }

    return EXIT_SUCCESS;
}
