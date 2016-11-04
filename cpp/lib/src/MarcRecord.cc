/** \brief Marc-Implementation
 *  \author Oliver Obenland (oliver.obenland@uni-tuebingen.de)
 *
 *  \copyright 2016 Universitätsbiblothek Tübingen.  All rights reserved.
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

#include "MarcRecord.h"
#include "MarcTag.h"
#include "util.h"
#include <iostream>


const size_t MarcRecord::FIELD_NOT_FOUND;

MarcRecord &MarcRecord::operator=(const MarcRecord &rhs) {
    if (likely(&rhs != this)) {
        leader_ = rhs.leader_;
        raw_data_ = rhs.raw_data_;
        directory_entries_ = rhs.directory_entries_;
    }
    return *this;
}

std::string MarcRecord::getFieldData(const MarcTag &tag) const {
    return getFieldData(getFieldIndex(tag));
}


std::string MarcRecord::getFieldData(const size_t index) const {
    if (directory_entries_.cbegin() + index >= directory_entries_.cend())
        return "";
    const DirectoryEntry &entry(directory_entries_[index]);
    return std::string(raw_data_, entry.getFieldOffset(), entry.getFieldLength() - 1);
}


Subfields MarcRecord::getSubfields(const MarcTag &tag) const {
    return getSubfields(getFieldIndex(tag));
}


Subfields MarcRecord::getSubfields(const size_t index) const {
    if (directory_entries_.cbegin() + index >= directory_entries_.cend())
        return Subfields();
    return Subfields(getFieldData(index));
}


void MarcRecord::deleteSubfield(const size_t field_index, const char subfield_code) {
    Subfields subfields(getSubfields(field_index));
    subfields.erase(subfield_code);
    updateField(field_index, subfields.toString());
}


MarcTag MarcRecord::getTag(const size_t index) const {
    if (directory_entries_.cbegin() + index >= directory_entries_.cend())
        return "";
    return directory_entries_[index].getTag();
}


size_t MarcRecord::getFieldIndex(const MarcTag &field_tag) const {
    for (size_t i(0); i < getNumberOfFields(); ++i) {
        if (directory_entries_[i].getTag() == field_tag)
            return i;
    }
    return MarcRecord::FIELD_NOT_FOUND;
}


size_t MarcRecord::getFieldIndices(const MarcTag &field_tag, std::vector <size_t> *const field_indices) const {
    field_indices->clear();

    size_t field_index(getFieldIndex(field_tag));
    while (static_cast<size_t>(field_index) < directory_entries_.size() and
           directory_entries_[field_index].getTag() == field_tag) {
        field_indices->emplace_back(field_index);
        ++field_index;
    }

    return field_indices->size();
}


bool MarcRecord::updateField(const size_t field_index, const std::string &new_field_value) {
    DirectoryEntry &entry(directory_entries_[field_index]);
    size_t offset = raw_data_.size();
    size_t length = new_field_value.length() + 1 /* For new field separator. */;

    entry.setFieldLength(length);
    entry.setFieldOffset(offset);
    raw_data_ += new_field_value + '\x1E';

    return true;
}


bool MarcRecord::insertSubfield(const MarcTag &new_field_tag, const char subfield_code,
                                const std::string &new_subfield_value, const char indicator1, const char indicator2) {
    return insertField(new_field_tag, std::string(1, indicator1) + std::string(1, indicator2) + "\x1F"
                                      + std::string(1, subfield_code) + new_subfield_value);
}


size_t MarcRecord::insertField(const MarcTag &new_field_tag, const std::string &new_field_value) {

    // Find the insertion location:
    auto insertion_location(directory_entries_.begin());
    while (insertion_location != directory_entries_.end() and new_field_tag >= insertion_location->getTag())
        ++insertion_location;

    const size_t offset(raw_data_.size());
    const size_t length(new_field_value.length() + 1) /* For new field separator. */;
    const auto inserted_location(directory_entries_.emplace(insertion_location, new_field_tag, length, offset));
    raw_data_ += new_field_value + '\x1E';

    const size_t index(std::distance(directory_entries_.begin(), inserted_location));
    return index;
}


void MarcRecord::deleteField(const size_t field_index) {
    directory_entries_.erase(directory_entries_.begin() + field_index);
}


void MarcRecord::deleteFields(const std::vector <std::pair<size_t, size_t>> &blocks) {
    std::vector <DirectoryEntry> new_entries;
    new_entries.reserve(directory_entries_.size());

    size_t copy_start(0);
    for (const std::pair <size_t, size_t> block : blocks) {
        new_entries.insert(new_entries.end(), directory_entries_.begin() + copy_start,
                           directory_entries_.begin() + block.first);
        copy_start = block.second;
    }
    new_entries.insert(new_entries.end(), directory_entries_.begin() + copy_start, directory_entries_.end());
    new_entries.swap(directory_entries_);
}


std::string MarcRecord::extractFirstSubfield(const MarcTag &tag, const char subfield_code) const {
    const size_t index(getFieldIndex(tag));
    if (index == FIELD_NOT_FOUND)
        return "";
    return getSubfields(tag).getFirstSubfieldValue(subfield_code);
}


std::string MarcRecord::extractFirstSubfield(const size_t field_index, const char subfield_code) const {
    const Subfields subfields(getFieldData(field_index));
    return subfields.getFirstSubfieldValue(subfield_code);
}


size_t MarcRecord::extractAllSubfields(const std::string &tags, std::vector <std::string> *const values,
                                       const std::string &ignore_subfield_codes) const {
    values->clear();

    std::vector <std::string> individual_tags;
    StringUtil::Split(tags, ':', &individual_tags);
    for (const auto &tag : individual_tags) {
        size_t field_index(getFieldIndex(tag));
        while (static_cast<size_t>(field_index) < directory_entries_.size() and
               directory_entries_[field_index].getTag() == tag) {
            const Subfields subfields(getSubfields(field_index));
            for (const auto &subfield : subfields) {
                if (ignore_subfield_codes.find(subfield.code_) == std::string::npos)
                    values->emplace_back(subfield.value_);
            }
            ++field_index;
        }
    }
    return values->size();
}


size_t MarcRecord::extractSubfield(const MarcTag &tag, const char subfield_code,
                                   std::vector <std::string> *const values) const {
    values->clear();

    size_t field_index(getFieldIndex(tag));
    while (static_cast<size_t>(field_index) < directory_entries_.size() and
           tag == directory_entries_[field_index].getTag()) {
        const Subfields subfields(getSubfields(field_index));
        const auto begin_end(subfields.getIterators(subfield_code));
        for (auto subfield_code_and_value(begin_end.first);
             subfield_code_and_value != begin_end.second; ++subfield_code_and_value)
            values->emplace_back(subfield_code_and_value->value_);
        ++field_index;
    }
    return values->size();
}


size_t MarcRecord::extractSubfields(const MarcTag &tag, const std::string &subfield_codes,
                                    std::vector <std::string> *const values) const {
    values->clear();

    size_t field_index(getFieldIndex(tag));
    while (static_cast<size_t>(field_index) < directory_entries_.size() and
           tag == directory_entries_[field_index].getTag()) {
        const Subfields subfields(getSubfields(field_index));
        for (const auto &subfield : subfields) {
            if (subfield_codes.find(subfield.code_) != std::string::npos)
                values->emplace_back(subfield.value_);
        }
        ++field_index;
    }

    return values->size();
}


size_t MarcRecord::findAllLocalDataBlocks(std::vector <std::pair<size_t, size_t>> *const local_block_boundaries) const {
    local_block_boundaries->clear();

    size_t local_block_start(getFieldIndex("LOK"));
    if (local_block_start == FIELD_NOT_FOUND)
        return 0;

    size_t local_block_end(local_block_start + 1);
    while (local_block_end < directory_entries_.size()) {
        if (StringUtil::StartsWith(getFieldData(local_block_end), "  ""\x1F""0000")) {
            local_block_boundaries->emplace_back(std::make_pair(local_block_start, local_block_end));
            local_block_start = local_block_end;
        }
        ++local_block_end;
    }
    local_block_boundaries->emplace_back(std::make_pair(local_block_start, local_block_end));

    return local_block_boundaries->size();
}


static bool IndicatorsMatch(const std::string &indicator_pattern, const std::string &indicators) {
    if (indicator_pattern[0] != '?' and indicator_pattern[0] != indicators[0])
        return false;
    if (indicator_pattern[1] != '?' and indicator_pattern[1] != indicators[1])
        return false;
    return true;
}


size_t MarcRecord::findFieldsInLocalBlock(const MarcTag &field_tag, const std::string &indicators,
                                          const std::pair <size_t, size_t> &block_start_and_end,
                                          std::vector <size_t> *const field_indices) const {
    field_indices->clear();
    if (unlikely(indicators.length() != 2))
        Error("in MarcUtil::FindFieldInLocalBlock: indicators must be precisely 2 characters long!");

    const std::string FIELD_PREFIX("  ""\x1F""0" + field_tag.to_string());
    for (size_t index(block_start_and_end.first); index < block_start_and_end.second; ++index) {
        const std::string &current_field(getFieldData(index));
        if (StringUtil::StartsWith(current_field, FIELD_PREFIX)
            and IndicatorsMatch(indicators, current_field.substr(7, 2)))
            field_indices->emplace_back(index);
    }
    return field_indices->size();
}


void MarcRecord::filterTags(const std::unordered_set <MarcTag> &drop_tags) {
    std::vector <std::pair<size_t, size_t>> deleted_blocks;
    for (auto entry = directory_entries_.begin(); entry < directory_entries_.end(); ++entry) {
        const auto tag_iter = drop_tags.find(entry->getTag());
        if (tag_iter == drop_tags.cend())
            continue;
        const size_t block_start = std::distance(directory_entries_.begin(), entry);
        for (/* empty */; entry < directory_entries_.end() and entry->getTag() == *tag_iter; ++entry);
        const size_t block_end = std::distance(directory_entries_.begin(), entry);

        deleted_blocks.emplace_back(block_start, block_end);
    }
    deleteFields(deleted_blocks);
}


std::string MarcRecord::getLanguage(const std::string &default_language_code) const {
    const std::string &language(extractFirstSubfield("041", 'a'));
    if (likely(not language.empty()))
        return language;
    return default_language_code;
}


std::string MarcRecord::getLanguageCode() const {
    const size_t _008_index(getFieldIndex("008"));
    if (_008_index == FIELD_NOT_FOUND)
        return "";
    // Language codes start at offset 35 and have a length of 3.
    const auto entry = directory_entries_[_008_index];
    if (entry.getFieldLength() < 38)
        return "";

    return std::string(raw_data_, entry.getFieldOffset() + 35, 3);
}


void MarcRecord::combine(const MarcRecord &record) {
    const size_t offset(raw_data_.size());
    raw_data_ += record.raw_data_;

    // Ignore first field. We only need one 001-field.
    directory_entries_.reserve(record.directory_entries_.size() - 1);
    for (auto iter(record.directory_entries_.begin() + 1); iter < record.directory_entries_.end(); ++iter) {
        directory_entries_.emplace_back(*iter);
        directory_entries_.back().setFieldOffset(iter->getFieldOffset() + offset);
    }
}


bool MarcRecord::ProcessRecords(File *const input, File *const output, RecordFunc process_record,
                                std::string *const err_msg) {
    err_msg->clear();

    while (MarcRecord record = MarcReader::Read(input)) {
        if (not (*process_record)(&record, output, err_msg))
            return false;
        err_msg->clear();
    }

    return err_msg->empty();
}


bool MarcRecord::ProcessRecords(File *const input, XmlRecordFunc process_record, XmlWriter *const xml_writer,
                                std::string *const err_msg) {
    err_msg->clear();

    while (MarcRecord record = MarcReader::ReadXML(input)) {
        if (not (*process_record)(&record, xml_writer, err_msg))
            return false;
        err_msg->clear();
    }

    return err_msg->empty();
}
