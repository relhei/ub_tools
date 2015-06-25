/** \brief Utility for deleting partial or entire MARC records based on an input list.
 *  \author Dr. Johannes Ruscheinski (johannes.ruscheinski@uni-tuebingen.de)
 *
 *  \copyright 2015 Universitätsbiblothek Tübingen.  All rights reserved.
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
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include "MarcUtil.h"
#include "StringUtil.h"
#include "Subfields.h"
#include "util.h"


static void Usage() __attribute__((noreturn));


static void Usage() {
    std::cerr << "Usage: " << progname << " deletion_list input_marc output_marc\n";
    std::exit(EXIT_FAILURE);
}


void ExtractDeletionIds(FILE * const deletion_list, std::unordered_set<std::string> * const title_deletion_ids,
			std::unordered_set<std::string> * const local_deletion_ids)
{
    char line[100];
    while (std::fgets(line, sizeof(line), deletion_list) != NULL) {
	if (std::strlen(line) < 13)
	    Error("short line in deletion list file: \"" + std::string(line) + "\"!");
	if (line[11] == 'A')
	    title_deletion_ids->insert(line + 12);
	else if (line[11] == '9')
	    local_deletion_ids->insert(line + 12);
    }

    std::fclose(deletion_list);
}


int MatchLocalID(const std::unordered_set<std::string> &local_ids, const std::vector<DirectoryEntry> &dir_entries,
		 const std::vector<std::string> &field_data)
{
    for (size_t i(0); i < dir_entries.size(); ++i) {
	if (dir_entries[i].getTag() != "LOK")
	    continue;

	const Subfields subfields(field_data[i]);
	if (not subfields.hasSubfield('0'))
	    continue;

	const std::string subfield_contents(subfields.getFirstSubfieldValue('0'));
	if (not StringUtil::StartsWith(subfield_contents, "001 ")
	    or local_ids.find(subfield_contents.substr(4)) == local_ids.end())
	    continue;

	return i;
    }

    return -1;
}


class MatchTag {
    const std::string tag_to_match_;
public:
    explicit MatchTag(const std::string &tag_to_match): tag_to_match_(tag_to_match) { }
    bool operator()(const DirectoryEntry &dir_entry) const { return dir_entry.getTag() == tag_to_match_; }
};


void ProcessRecords(const std::unordered_set<std::string> &title_deletion_ids,
		    const std::unordered_set<std::string> &local_deletion_ids, FILE * const input,
		    FILE * const output)
{
    Leader *raw_leader;
    std::vector<DirectoryEntry> dir_entries;
    std::vector<std::string> field_data;
    std::string err_msg;
    unsigned total_record_count(0), deleted_record_count(0), modified_record_count(0);

    while (MarcUtil::ReadNextRecord(input, &raw_leader, &dir_entries, &field_data, &err_msg)) {
	++total_record_count;

	if (dir_entries[0].getTag() != "001")
	    Error("First field is not \"001\"!");

	ssize_t start_local_match;
	if (title_deletion_ids.find(field_data[0]) != title_deletion_ids.end()) {
	    MarcUtil::ComposeAndWriteRecord(output, dir_entries, field_data, raw_leader);
	
	    bool modified(false);
	    while ((start_local_match = MatchLocalID(local_deletion_ids, dir_entries, field_data)) != -1) {
		// We now expect a field "000" before the current "001" field:
		--start_local_match;
		if (start_local_match <= 0)
		    Error("weird data structure (1)!");
		const Subfields subfields(field_data[start_local_match]);
		if (not subfields.hasSubfield('0')
		    or StringUtil::StartsWith(subfields.getFirstSubfieldValue('0'), "000 "))
		    Error("missing or empty local field \"000\"!");

		// Now we need to find the index one past the end of the local record.  This would
		// be either the "000" field of the next local record or one past the end of the overall
		// MARC record.
		bool found_next_000(false);
		size_t end_local_match(start_local_match + 2);
		while (end_local_match < field_data.size()) {
		    const Subfields subfields(field_data[end_local_match]);
		    if (not subfields.hasSubfield('0'))
			Error("weird data (2)!");
		    if (StringUtil::StartsWith(subfields.getFirstSubfieldValue('0'), "000 ")) {
			found_next_000 = true;
			break;
		    }

		    ++end_local_match;
		}
		if (not found_next_000)
		    ++end_local_match;

		// Throw away the matched local data set:
		dir_entries.erase(dir_entries.begin() + start_local_match, dir_entries.begin() + end_local_match);

		modified = true;
	    }

	    if (not modified)
		MarcUtil::ComposeAndWriteRecord(output, dir_entries, field_data, raw_leader);
	    else {
		// Only keep records that still have at least one "LOK" tag:
		if (std::find_if(dir_entries.cbegin(), dir_entries.cend(), MatchTag("LOK")) == dir_entries.cend())
		    ++deleted_record_count;
		else {
		    ++modified_record_count;
		    MarcUtil::ComposeAndWriteRecord(output, dir_entries, field_data, raw_leader);
		}
	    }
	} else {
	    ++deleted_record_count;
	    std::cout << "Deleted record with ID " << field_data[0] << '\n';
	}
    }

    if (not err_msg.empty())
	Error(err_msg);
    std::cerr << "Read " << total_record_count << " records.\n";
    std::cerr << "Deleted " << deleted_record_count << " records.\n";
    std::cerr << "Modified " << modified_record_count << " records.\n";

    std::fclose(input);
    std::fclose(output);
}


int main(int argc, char *argv[]) {
    progname = argv[0];

    if (argc != 4)
	Usage();

    const std::string deletion_list_filename(argv[1]);
    FILE *deletion_list = std::fopen(deletion_list_filename.c_str(), "rb");
    if (deletion_list == NULL)
	Error("can't open \"" + deletion_list_filename + "\" for reading!");

    std::unordered_set<std::string> title_deletion_ids, local_deletion_ids;
    ExtractDeletionIds(deletion_list, &title_deletion_ids, &local_deletion_ids);

    const std::string marc_input_filename(argv[2]);
    FILE *marc_input = std::fopen(marc_input_filename.c_str(), "rb");
    if (marc_input == NULL)
	Error("can't open \"" + marc_input_filename + "\" for reading!");

    const std::string marc_output_filename(argv[3]);
    FILE *marc_output = std::fopen(marc_output_filename.c_str(), "wb");
    if (marc_output == NULL)
	Error("can't open \"" + marc_output_filename + "\" for writing!");

    try {
	ProcessRecords(title_deletion_ids, local_deletion_ids, marc_input, marc_output);
    } catch (const std::exception &e) {
	Error("Caught exception: " + std::string(e.what()));
    }
}
