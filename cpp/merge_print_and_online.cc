/** \brief Utility for merging print and online editions into single records.
 *  \author Dr. Johannes Ruscheinski (johannes.ruscheinski@uni-tuebingen.de)
 *
 *  \copyright 2018 Universitätsbibliothek Tübingen.  All rights reserved.
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
#include <set>
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
#include "util.h"
#include "VuFind.h"


namespace {


[[noreturn]] void Usage() {
    std::cerr << "Usage: " << ::progname
              << " [--debug] marc_input marc_output missing_ppn_partners_list\n"
              << "       missing_ppn_partners_list will be generated by this program and will contain the PPN's\n"
              << "       of superior works with cross links between print and online edition with one of\n"
              << "       the partners missing.  N.B. the input MARC file *must* be in the MARC-21 format!\n\n";
    std::exit(EXIT_FAILURE);
}


std::string NormaliseISSN(const std::string &issn) {
    if (issn.length() == 8)
        return issn;
    if (issn.length() == 9 and issn[4] == '-')
        return issn.substr(0, 4) + issn.substr(5);
    LOG_WARNING("Strange ISSN \"" + issn + "\"!");
    return issn;
}


const std::vector<std::string> ISSN_SUBFIELDS{ "022a", "029a", "440x", "490x", "730x", "773x", "776x", "780x", "785x" };


void PopulateISSNtoControlNumberMap(MARC::Reader * const marc_reader,
                                    std::unordered_map<std::string, std::string> * const issn_to_control_number_map)
{
    unsigned count(0);
    while (const MARC::Record record = marc_reader->read()) {
        if (not record.isSerial())
            continue;

        bool found_at_least_one_issn(false);
        for (const std::string &issn_subfield : ISSN_SUBFIELDS) {
            for (const auto &field : record.getTagRange(issn_subfield.substr(0, MARC::Record::TAG_LENGTH))) {
                const MARC::Subfields subfields(field.getSubfields());
                for (const auto &subfield_value : subfields.extractSubfields(issn_subfield[MARC::Record::TAG_LENGTH])) {
                    issn_to_control_number_map->emplace(NormaliseISSN(subfield_value), record.getControlNumber());
                    found_at_least_one_issn = true;
                }
            }
        }

        if (found_at_least_one_issn)
            ++count;
    }

    LOG_INFO("Found " + std::to_string(issn_to_control_number_map->size()) + " ISSN's associated with " + std::to_string(count)
             + " record(s).");
}


// Creates a map from the PPN "partners" to the offsets of the original records that had the links to the partners.
void CollectMappings(const bool debug, MARC::Reader * const marc_reader, File * const missing_partners,
                     const std::unordered_map<std::string, std::string> &issn_to_control_number_map,
                     std::unordered_map<std::string, off_t> * const control_number_to_offset_map,
                     std::unordered_map<std::string, std::string> * const ppn_to_ppn_map)
{
    std::unordered_set<std::string> all_ppns;
    off_t last_offset(marc_reader->tell());
    unsigned count(0);
    while (const MARC::Record record = marc_reader->read()) {
        ++count;
        all_ppns.emplace(record.getControlNumber());

        bool found_a_partner(false);
        for (const auto &field : record.getTagRange("776")) {
            const MARC::Subfields _776_subfields(field.getSubfields());
            if (_776_subfields.getFirstSubfieldWithCode('i') == "Erscheint auch als") {
                for (const auto &w_subfield : _776_subfields.extractSubfields('w')) {
                    if (StringUtil::StartsWith(w_subfield, "(DE-576)")) {
                        const std::string other_ppn(w_subfield.substr(__builtin_strlen("(DE-576)")));
                        (*control_number_to_offset_map)[other_ppn] = last_offset;
                        if (debug)
                            LOG_INFO("Partner of " + other_ppn + " is " + record.getControlNumber() + ".");

                        // Consistently use the alphanumerically smaller PPN as the key in our map:
                        if (other_ppn < record.getControlNumber())
                            (*ppn_to_ppn_map)[other_ppn] = record.getControlNumber();
                        else
                            (*ppn_to_ppn_map)[record.getControlNumber()] = other_ppn;

                        found_a_partner = true;
                    }
                }
            }
        }

        if (not found_a_partner) {
            for (const auto &_029_field : record.getTagRange("029")) {
                if (_029_field.getIndicator1() != 'x'
                    or (_029_field.getIndicator2() != 'c' and _029_field.getIndicator2() != 'd'))
                    continue;

                const MARC::Subfields subfields(_029_field.getSubfields());
                if (not subfields.hasSubfield('a'))
                    continue;

                const auto issn_and_ppn(issn_to_control_number_map.find(NormaliseISSN(subfields.getFirstSubfieldWithCode('a'))));
                if (issn_and_ppn == issn_to_control_number_map.end())
                    continue;

                if (issn_and_ppn->second < record.getControlNumber())
                    (*ppn_to_ppn_map)[issn_and_ppn->second] = record.getControlNumber();
                else
                    (*ppn_to_ppn_map)[record.getControlNumber()] = issn_and_ppn->second;
            }
        }

        last_offset = marc_reader->tell();
    }

    unsigned no_partner_count(0);
    auto control_number_and_offset(control_number_to_offset_map->begin());
    while (control_number_and_offset != control_number_to_offset_map->end()) {
        if (all_ppns.find(control_number_and_offset->first) != all_ppns.end())
            ++control_number_and_offset;
        else {
            ++no_partner_count;
            missing_partners->write(control_number_and_offset->first + "\n");
            control_number_and_offset = control_number_to_offset_map->erase(control_number_and_offset);
        }
    }

    LOG_INFO("Found " + std::to_string(count) + " record(s).");
    LOG_INFO("Found " + std::to_string(control_number_to_offset_map->size())
             + " superior record(s) that we may be able to merge.");
    LOG_INFO("Found " + std::to_string(no_partner_count) + " superior record(s) that have missing \"partners\".");
}


// Make inferior works point to the new merged superior parent found in "ppn_to_ppn_map".
bool PatchUplink(MARC::Record * const record, const std::unordered_map<std::string, std::string> &ppn_to_ppn_map) {
    static const std::set<std::string> UPLINK_TAGS{ "800", "810", "830", "773", "776" };

    bool patched(false);
    for (auto field : *record) {
        if (UPLINK_TAGS.find(field.getTag().toString()) != UPLINK_TAGS.cend()) {
            MARC::Subfields subfields(field.getSubfields());
            auto subfield_w(std::find_if(subfields.begin(), subfields.end(),
                                         [](const MARC::Subfield &subfield) -> bool { return subfield.code_ == 'w'; }));
            if (subfield_w == subfields.end())
                continue;
            if (not StringUtil::StartsWith(subfield_w->value_, "(DE-576)"))
                continue;
            const std::string uplink_ppn(subfield_w->value_.substr(__builtin_strlen("(DE-576)")));
            const auto uplink_ppns(ppn_to_ppn_map.find(uplink_ppn));
            if (uplink_ppns == ppn_to_ppn_map.end())
                continue;

            // If we made it here, we need to replace the uplink PPN:
            subfield_w->value_ = "(DE-576)" + uplink_ppns->second;
            field.setContents(std::string(1, field.getIndicator1()) + std::string(1, field.getIndicator2())
                              + subfields.toString());
            patched = true;
        }
    }

    return patched;
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
    for (auto subfield1(subfields1.begin()), subfield2(subfields2.begin()); subfield1 != subfields1.end();
         ++subfield1, ++subfield2)
    {
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


MARC::Record::Field MergeControlFields(const MARC::Tag &tag, const std::string &field_contents1,
                                       const std::string &field_contents2)
{
    std::string merged_contents;

    if (tag == "005") // Date and Time of Latest Transaction
        merged_contents = std::max(field_contents1, field_contents2);
    else
        merged_contents = field_contents1;

    return MARC::Record::Field(tag, merged_contents);
}


// Returns true if the contents of the leading subfields with subfield codes "subfield_codes" in field1 and field2 are
// identical, else returns false. Please note that the code specified in "subfield_codes" must exist.
bool SubfieldPrefixIsIdentical(const MARC::Record::Field &field1, const MARC::Record::Field &field2,
                               const std::vector<char> &subfield_codes)
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
        if (subfield1->value_ != subfield2->value_)
            return false;
    }

    return true;
}


MARC::Record MergeRecords(MARC::Record &record1, MARC::Record &record2) {
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
        } else if (record1_field->getTag() == record2_field->getTag() and record1_field->getTag() == "264"
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

    // Mark the record as being both "print" as well as "electronic" and store the PPN of the dropped record:
    merged_record.insertField("ZWI", { { 'a', "1" }, { 'b', record2.getControlNumber() } });
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
            _246_field.setContents(_246_subfields, _246_field.getIndicator1(), _246_field.getIndicator2());
    }

    return *record;
}


void ProcessRecords(const bool /*debug*/, MARC::Reader * const marc_reader, MARC::Writer * const marc_writer,
                    const std::unordered_map<std::string, off_t> &control_number_to_offset_map,
                    const std::unordered_map<std::string, std::string> &ppn_to_ppn_map)
{
    std::unordered_set<std::string> skip_ppns;
    for (const auto &from_ppn_and_to_ppn : ppn_to_ppn_map)
        skip_ppns.emplace(from_ppn_and_to_ppn.second);

    unsigned record_count(0), merged_count(0), patched_uplink_count(0);
    while (MARC::Record record = marc_reader->read()) {
        ++record_count;

        if (skip_ppns.find(record.getControlNumber()) != skip_ppns.end())
            continue;

        const auto control_number_and_offset(control_number_to_offset_map.find(record.getControlNumber()));
        if (control_number_and_offset != control_number_to_offset_map.end()) {
            MARC::Record record2(ReadRecordFromOffsetOrDie(marc_reader, control_number_and_offset->second));
            record = MergeRecords(Patch246i(&record), Patch246i(&record2));
            ++merged_count;
        } else if (PatchUplink(&record, ppn_to_ppn_map))
            ++patched_uplink_count;

        marc_writer->write(record);
    }

    LOG_INFO("Data set contained " + std::to_string(record_count) + " MARC record(s).");
    LOG_INFO("Merged " + std::to_string(merged_count) + " MARC record(s).");
    LOG_INFO("Patched uplinks of " + std::to_string(patched_uplink_count) + " MARC record(s).");
}


// Here we update subscriptions.  There are 3 possible cases for each user and mapped PPN:
// 1. The trivial case where no subscriptions exist for a mapped PPN.
// 2. A subscription only exists for the mapped PPN.
//    In this case we only have to swap the PPN for the subscription.
// 3. Subscriptions exist for both, electronic and print PPNs.
//    Here we have to delete the subscription for the mapped PPN and ensure that the max_last_modification_time of the
//    remaining subscription is the minimum of the two previously existing subscriptions.
void PatchSerialSubscriptions(DbConnection * connection, const std::unordered_map<std::string, std::string> &ppn_to_ppn_map) {
    for (const auto &ppn_and_ppn : ppn_to_ppn_map) {
        connection->queryOrDie("SELECT id,max_last_modification_time FROM ixtheo_journal_subscriptions WHERE "
                               "journal_control_number='" + ppn_and_ppn.first + "'");
        DbResultSet ppn_first_result_set(connection->getLastResultSet());
        while (const DbRow ppn_first_row = ppn_first_result_set.getNextRow()) {
            const std::string user_id(ppn_first_row["id"]);
            connection->queryOrDie("SELECT max_last_modification_time FROM ixtheo_journal_subscriptions "
                                   "WHERE id='" + user_id + "' AND journal_control_number='" + ppn_and_ppn.second + "'");
            DbResultSet ppn_second_result_set(connection->getLastResultSet());
            if (ppn_second_result_set.empty()) {
                connection->queryOrDie("UPDATE ixtheo_journal_subscriptions SET journal_control_number='"
                                       + ppn_and_ppn.second + "' WHERE id='" + user_id + "' AND journal_control_number='"
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
            connection->queryOrDie("DELETE FROM ixtheo_journal_subscriptions WHERE journal_control_number='"
                                   + ppn_and_ppn.first + "' and id='" + user_id + "'");
            if (ppn_first_row["max_last_modification_time"] > min_max_last_modification_time)
                connection->queryOrDie("UPDATE ixtheo_journal_subscriptions SET max_last_modification_time='"
                                       + min_max_last_modification_time + "' WHERE journal_control_number='"
                                       + ppn_and_ppn.second + "' and id='" + user_id + "'");
        }
    }
}


void PatchPDASubscriptions(DbConnection * connection, const std::unordered_map<std::string, std::string> &ppn_to_ppn_map) {
    for (const auto &ppn_and_ppn : ppn_to_ppn_map) {
        connection->queryOrDie("SELECT id FROM ixtheo_pda_subscriptions WHERE book_ppn='" + ppn_and_ppn.first + "'");
        DbResultSet result_set(connection->getLastResultSet());
        while (const DbRow row = result_set.getNextRow())
            connection->queryOrDie("UPDATE ixtheo_pda_subscriptions SET book_ppn='" + ppn_and_ppn.first + "' WHERE id='"
                                   + row["id"] + "' AND book_ppn='" + ppn_and_ppn.second + "'");
    }
}


void PatchResourceTable(DbConnection * connection, const std::unordered_map<std::string, std::string> &ppn_to_ppn_map) {
    for (const auto &ppn_and_ppn : ppn_to_ppn_map) {
        connection->queryOrDie("SELECT id FROM resource WHERE record_id='" + ppn_and_ppn.first + "'");
        DbResultSet result_set(connection->getLastResultSet());
        while (const DbRow row = result_set.getNextRow())
            connection->queryOrDie("UPDATE resource SET record_id='" + ppn_and_ppn.second + "' WHERE id=" + row["id"]);
    }
}


} // unnamed namespace


int main(int argc, char *argv[]) {
    ::progname = argv[0];

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

    try {
        std::unordered_map<std::string, std::string> issn_to_control_number_map;
        PopulateISSNtoControlNumberMap(marc_reader.get(), &issn_to_control_number_map);
        marc_reader->rewind();

        std::unordered_map<std::string, off_t> control_number_to_offset_map;
        std::unordered_map<std::string, std::string> ppn_to_ppn_map;
        CollectMappings(debug, marc_reader.get(), missing_partners.get(), issn_to_control_number_map,
                        &control_number_to_offset_map, &ppn_to_ppn_map);
        marc_reader->rewind();
        ProcessRecords(debug, marc_reader.get(), marc_writer.get(), control_number_to_offset_map, ppn_to_ppn_map);

        if (not debug) {
            std::string mysql_url;
            VuFind::GetMysqlURL(&mysql_url);
            DbConnection db_connection(mysql_url);
            PatchSerialSubscriptions(&db_connection, ppn_to_ppn_map);
            PatchPDASubscriptions(&db_connection, ppn_to_ppn_map);
            PatchResourceTable(&db_connection, ppn_to_ppn_map);
        }
    } catch (const std::exception &e) {
        LOG_ERROR("Caught exception: " + std::string(e.what()));
    }
}
