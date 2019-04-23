/** \file    patch_up_ppns_for_k10plus.cc
 *  \brief   Swaps out all persistent old PPN's with new PPN's.
 *  \author  Dr. Johannes Ruscheinski
 */

/*
    Copyright (C) 2019, Library of the University of Tübingen

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
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <kchashdb.h>
#include "Compiler.h"
#include "DbConnection.h"
#include "DbResultSet.h"
#include "DbRow.h"
#include "MapUtil.h"
#include "MARC.h"
#include "RegexMatcher.h"
#include "StringUtil.h"
#include "UBTools.h"
#include "util.h"
#include "VuFind.h"


namespace {


[[noreturn]] void Usage() {
    ::Usage("[--store-only] marc_input1 [marc_input2 .. marc_inputN]"
            "If --store-only has been specified, no swapping will be performed and only the persistent map file will be overwritten.");
}


struct PPNsAndSigil {
    std::string old_ppn_, old_sigil_, new_ppn_;
public:
    PPNsAndSigil(const std::string &old_ppn, const std::string &old_sigil, const std::string &new_ppn)
        : old_ppn_(old_ppn), old_sigil_(old_sigil), new_ppn_(new_ppn) { }
    PPNsAndSigil() = default;
    PPNsAndSigil(const PPNsAndSigil &other) = default;
};


void LoadMapping(MARC::Reader * const marc_reader,
                 const std::unordered_multimap<std::string, std::string> &already_processed_ppns_and_sigils,
                 std::vector<PPNsAndSigil> * const old_ppns_sigils_and_new_ppns)
{
    // We need to consider the sigil of the old BSZ as well as the K10+ sigil because in future there may also be merges of
    // K10+ entities which will lead to old PPN's being K10+ PPN's.
    auto matcher(RegexMatcher::RegexMatcherFactoryOrDie("^\\((DE-576|DE-627)\\)(.+)"));
    while (const auto record = marc_reader->read()) {
        for (const auto &field : record.getTagRange("035")) {
            const auto subfield_a(field.getFirstSubfieldWithCode('a'));
            if (matcher->matched(subfield_a)) {
                const std::string old_sigil((*matcher)[1]);
                const std::string old_ppn((*matcher)[2]);
                if (not MapUtil::Contains(already_processed_ppns_and_sigils, old_ppn, old_sigil))
                    old_ppns_sigils_and_new_ppns->emplace_back(old_ppn, old_sigil, record.getControlNumber());
            }
        }
    }

    LOG_INFO("Found " + std::to_string(old_ppns_sigils_and_new_ppns->size()) + " new mappings of old PPN's to new PPN's in \""
             + marc_reader->getPath() + "\".\n");
}


void PatchTable(DbConnection * const db_connection, const std::string &table, const std::string &column,
                const std::vector<PPNsAndSigil> &old_ppns_sigils_and_new_ppns)
{
    const unsigned MAX_BATCH_SIZE(100);

    db_connection->queryOrDie("BEGIN");

    unsigned replacement_count(0), batch_size(0);
    for (const auto &old_ppn_sigil_and_new_ppn : old_ppns_sigils_and_new_ppns) {
        ++batch_size;
        db_connection->queryOrDie("UPDATE IGNORE " + table + " SET " + column + "='" + old_ppn_sigil_and_new_ppn.new_ppn_
                                  + "' WHERE " + column + "='" + old_ppn_sigil_and_new_ppn.old_ppn_ + "'");
        replacement_count += db_connection->getNoOfAffectedRows();
        if (batch_size >= MAX_BATCH_SIZE) {
            db_connection->queryOrDie("COMMIT");
            db_connection->queryOrDie("BEGIN");
        }
    }

    db_connection->queryOrDie("COMMIT");

    LOG_INFO("Replaced " + std::to_string(replacement_count) + " rows in " + table + ".");
}


void PatchNotifiedDB(const std::string &user_type, const std::vector<PPNsAndSigil> &old_ppns_sigils_and_new_ppns) {
    const std::string DB_FILENAME(UBTools::GetTuelibPath() + user_type + "_notified.db");
    std::unique_ptr<kyotocabinet::HashDB> db(new kyotocabinet::HashDB());
    if (not (db->open(DB_FILENAME, kyotocabinet::HashDB::OWRITER | kyotocabinet::HashDB::OREADER))) {
        LOG_INFO("\"" + DB_FILENAME + "\" not found!");
        return;
    }

    unsigned updated_count(0);
    for (const auto &ppns_and_sigil : old_ppns_sigils_and_new_ppns) {
        std::string value;
        if (db->get(ppns_and_sigil.old_ppn_, &value)) {
            if (unlikely(not db->remove(ppns_and_sigil.old_ppn_)))
                LOG_ERROR("failed to remove key \"" + ppns_and_sigil.old_ppn_ + "\" from \"" + DB_FILENAME + "\"!");
            if (unlikely(not db->add(ppns_and_sigil.old_ppn_, value)))
                LOG_ERROR("failed to add key \"" + ppns_and_sigil.old_ppn_ + "\" from \"" + DB_FILENAME + "\"!");
            ++updated_count;
        }
    }

    LOG_INFO("Updated " + std::to_string(updated_count) + " entries in \"" + DB_FILENAME + "\".");
}


bool HaveAllPermissions(DbConnection * const db_connection, const std::string &database) {
    db_connection->queryOrDie("SHOW GRANTS FOR '" + db_connection->getUser() + "'@'" + db_connection->getHost() + "'");
    DbResultSet result_set(db_connection->getLastResultSet());
    while (const auto row = result_set.getNextRow()) {
        if (row[0]
            == "GRANT ALL PRIVILEGES ON `" + database + "`.* TO '" + db_connection->getUser() + "'@'" + db_connection->getHost() + "'")
            return true;
    }
    return false;
}


void CheckMySQLPermissions(DbConnection * const db_connection) {
    if (not HaveAllPermissions(db_connection, "vufind"))
        LOG_ERROR("'" + db_connection->getUser() + "'@'" + db_connection->getHost() + "' needs all permissions on the vufind database!");
    if (VuFind::GetTueFindFlavour() == "ixtheo") {
        if (not HaveAllPermissions(db_connection, "ixtheo"))
            LOG_ERROR("'" + db_connection->getUser() + "'@' " + db_connection->getHost()
                      + "' needs all permissions on the ixtheo database!");
    }
}


void AddPPNsAndSigilsToMultiMap(const std::vector<PPNsAndSigil> &old_ppns_sigils_and_new_ppns,
                                std::unordered_multimap<std::string, std::string> * const already_processed_ppns_and_sigils)
{
    for (const auto &old_ppn_sigil_and_new_ppn : old_ppns_sigils_and_new_ppns)
        already_processed_ppns_and_sigils->emplace(std::make_pair(old_ppn_sigil_and_new_ppn.old_ppn_, old_ppn_sigil_and_new_ppn.old_sigil_));
}


} // unnamed namespace


static const std::string ALREADY_SWAPPED_PPNS_MAP_FILE(UBTools::GetTuelibPath() + "k10+_ppn_map.map");


int Main(int argc, char **argv) {
    if (argc < 2)
        Usage();

    bool store_only(false);
    if (std::strcmp(argv[1], "--store-only") == 0) {
        store_only = true;
        --argc, ++argv;
        if (argc < 2)
            Usage();
    }

    DbConnection db_connection; // ub_tools user

    CheckMySQLPermissions(&db_connection);

    std::unordered_multimap<std::string, std::string> already_processed_ppns_and_sigils;
    if (not store_only)
        MapUtil::DeserialiseMap(ALREADY_SWAPPED_PPNS_MAP_FILE, &already_processed_ppns_and_sigils);

    std::vector<PPNsAndSigil> old_ppns_sigils_and_new_ppns;
    for (int arg_no(1); arg_no < argc; ++arg_no) {
        const auto marc_reader(MARC::Reader::Factory(argv[arg_no]));
        LoadMapping(marc_reader.get(), already_processed_ppns_and_sigils, &old_ppns_sigils_and_new_ppns);
    }
    if (old_ppns_sigils_and_new_ppns.empty()) {
        LOG_INFO("nothing to do!");
        return EXIT_SUCCESS;
    }

    if (store_only) {
        AddPPNsAndSigilsToMultiMap(old_ppns_sigils_and_new_ppns, &already_processed_ppns_and_sigils);
        MapUtil::SerialiseMap(ALREADY_SWAPPED_PPNS_MAP_FILE, already_processed_ppns_and_sigils);
        return EXIT_SUCCESS;
    }

    PatchNotifiedDB("ixtheo", old_ppns_sigils_and_new_ppns);
    PatchNotifiedDB("relbib", old_ppns_sigils_and_new_ppns);

    PatchTable(&db_connection, "vufind.resource", "record_id", old_ppns_sigils_and_new_ppns);
    PatchTable(&db_connection, "vufind.record", "record_id", old_ppns_sigils_and_new_ppns);
    PatchTable(&db_connection, "vufind.change_tracker", "id", old_ppns_sigils_and_new_ppns);
    if (VuFind::GetTueFindFlavour() == "ixtheo") {
        PatchTable(&db_connection, "ixtheo.keyword_translations", "ppn", old_ppns_sigils_and_new_ppns);
        PatchTable(&db_connection, "vufind.ixtheo_journal_subscriptions", "journal_control_number_or_bundle_name",
                   old_ppns_sigils_and_new_ppns);
        PatchTable(&db_connection, "vufind.ixtheo_pda_subscriptions", "book_ppn", old_ppns_sigils_and_new_ppns);
        PatchTable(&db_connection, "vufind.relbib_ids", "record_id", old_ppns_sigils_and_new_ppns);
        PatchTable(&db_connection, "vufind.bibstudies_ids", "record_id", old_ppns_sigils_and_new_ppns);
    }

    AddPPNsAndSigilsToMultiMap(old_ppns_sigils_and_new_ppns, &already_processed_ppns_and_sigils);
    MapUtil::SerialiseMap(ALREADY_SWAPPED_PPNS_MAP_FILE, already_processed_ppns_and_sigils);

    return EXIT_SUCCESS;
}