/** \brief Utility for counting references to GND numbers.
 *  \author Dr. Johannes Ruscheinski (johannes.ruscheinski@uni-tuebingen.de)
 *
 *  \copyright 2017 Universitätsbiblothek Tübingen.  All rights reserved.
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
#include <cstdlib>
#include "FileUtil.h"
#include "MarcReader.h"
#include "MarcRecord.h"
#include "StringUtil.h"
#include "util.h"


static void Usage() __attribute__((noreturn));


static void Usage() {
    std::cerr << "Usage: " << ::progname << " gnd_number_list marc_data counts\n";
    std::exit(EXIT_FAILURE);
}


void LoadGNDNumbers(File * const input, std::unordered_map<std::string, unsigned> * const gnd_numbers_and_counts) {
    while (not input->eof()) {
        std::string line;
        if (input->getline(&line) > 0)
            gnd_numbers_and_counts->emplace(line, 0);
    }

    std::cout << "Loaded " << gnd_numbers_and_counts->size() << " GND numbers.\n";
}


void ProcessRecords(MarcReader * const marc_reader,
                    std::unordered_map<std::string, unsigned> * const gnd_numbers_and_counts)
{
    while (const MarcRecord record = marc_reader->read()) {
        const std::string _100_contents(record.getFieldData("100"));
        if (_100_contents.empty())
            continue;

        const Subfields subfields(_100_contents);
        const auto begin_end(subfields.getIterators('0'));
        for (auto subfield0(begin_end.first); subfield0 != begin_end.second; ++subfield0) {
            if (not StringUtil::StartsWith(subfield0->value_, "(DE-588)"))
                continue;

            const auto gnd_number_and_count(gnd_numbers_and_counts->find(subfield0->value_));
            if (gnd_number_and_count != gnd_numbers_and_counts->end())
                ++gnd_number_and_count->second;
        }
    }
}


void WriteCounts(const std::unordered_map<std::string, unsigned> &gnd_numbers_and_counts, File * const output) {
    for (const auto &gnd_number_and_count : gnd_numbers_and_counts) {
        if (gnd_number_and_count.second > 0)
            (*output) << gnd_number_and_count.first.substr(8) << '|' << gnd_number_and_count.second << '\n';
    }
}


int main(int argc, char *argv[]) {
    ::progname = argv[0];

    if (argc != 4)
        Usage();

    try {
        std::unique_ptr<File> gnd_numbers_and_counts_file(FileUtil::OpenInputFileOrDie(argv[1]));
        std::unordered_map<std::string, unsigned> gnd_numbers_and_counts;
        LoadGNDNumbers(gnd_numbers_and_counts_file.get(), &gnd_numbers_and_counts);
        
        std::unique_ptr<MarcReader> marc_reader(MarcReader::Factory(argv[2]));
        ProcessRecords(marc_reader.get(), &gnd_numbers_and_counts);

        std::unique_ptr<File> counts_file(FileUtil::OpenOutputFileOrDie(argv[3]));
        WriteCounts(gnd_numbers_and_counts, counts_file.get());
    } catch (const std::exception &e) {
        Error("Caught exception: " + std::string(e.what()));
    }
}