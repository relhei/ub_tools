/** \file   DirectoryEntry.h
 *  \brief  Interface for the DirectoryEntry class.
 *  \author Dr. Johannes Ruscheinski (johannes.ruscheinski@uni-tuebingen.de)
 *
 *  \copyright 2014 Universitätsbiblothek Tübingen.  All rights reserved.
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
#ifndef DIRECTORY_ENTRY_H
#define DIRECTORY_ENTRY_H


#include <string>
#include <utility>
#include <vector>
#include "StringUtil.h"


/** \class DirectoryEntry
 *  \brief Encapsulates a MARC-21 directory entry.
 */
class DirectoryEntry {
public:
    static const size_t DIRECTORY_ENTRY_LENGTH; //< The fixed length of a directory entry according to the standard.
    static const size_t TAG_LENGTH;             //< The fixed length of a field tag according to the standard.
private:
    std::string tag_;
    unsigned field_length_;
    unsigned field_offset_;
public:
    /** Constructs a DirectoryEntry from the binary representation of a MARC-21 directory entry. */
    explicit DirectoryEntry(const std::string &raw_entry);

    /** Copy constructor. */
    DirectoryEntry(const DirectoryEntry &other)
        : tag_(other.tag_), field_length_(other.field_length_), field_offset_(other.field_offset_) { }

    /** Move constructor. */
    DirectoryEntry(DirectoryEntry &&other)
	: field_length_(other.field_length_), field_offset_(other.field_offset_)
    {
	tag_.swap(other.tag_);
    }

    /** \brief Constructs a DirectoryEntry from its component parts.
     *
     *  \param tag           A field tag.  Must have a length of 3.
     *  \param field_length  Must be less than 10,000.
     *  \param field_offset  Must be less than 10,000.
     */
    DirectoryEntry(const std::string &tag, const unsigned field_length, const unsigned field_offset)
        : tag_(tag), field_length_(field_length), field_offset_(field_offset) {}

    inline DirectoryEntry &operator=(DirectoryEntry &&other) {
        tag_ = std::move(other.tag_);
        field_length_ = other.field_length_;
        field_offset_ = other.field_offset_;
        return *this;
    }

    const std::string &getTag() const { return tag_; }

    /** Includes the field terminator. */
    unsigned getFieldLength() const { return field_length_; }

    /** \param new_field_length  Must be less than 10,000 and must include the field terminator. */
    void setFieldLength(const unsigned new_field_length) { field_length_ = new_field_length; }

    unsigned getFieldOffset() const { return field_offset_; }

    /** \param new_field_offset  Must be less than 10,000. */
    void setFieldOffset(const unsigned new_field_offset) { field_offset_ = new_field_offset; }

    /** \return True if this DirectoryEntry corresponds to a control field, else false. */
    bool isControlFieldEntry() const { return StringUtil::StartsWith(tag_, "00"); }

    // Returns the string representation of a DirectoryEntry but w/o the trailing field terminator.
    std::string toString() const;

    /** \brief Parses a binary MARC-21 directory blob.
     *
     *  \param entries_string A binary blob that represents the directory of a MARC-21 record.
     *  \param entries        Return value containing the parsed DirectoryEntry's.
     *  \param err_msg        If not nullptr, error messages will be returned here.
     *
     *  \return True if no parse errors occurred, else false.
     */
    static bool ParseDirEntries(const std::string &entries_string, std::vector<DirectoryEntry> * const entries,
                                std::string * const err_msg = nullptr);

    /** \brief Locate the first occurrence of a field tag in a vector of DirectoryEntry's.
     *
     *  \param tag           The field tag we're looking for.
     *  \param field_entries The vector we're looking in.
     *
     *  \return An iterator referencing the first occurrence of the tag we were looking for or
     *          field_entries.end() if there were no n occurrences of the tag in "field_entries".
     */
    static std::vector<DirectoryEntry>::const_iterator FindField(const std::string &tag,
                                                                 const std::vector<DirectoryEntry> &field_entries);

    /** \brief Locate all occurrences of a field tag in a vector of DirectoryEntry's.
     *
     *  \param tag           The field tag we're looking for.
     *  \param field_entries The vector we're looking in.
     *
     *  \return An iterator pair indicating the range of the entries that match the tag.  The pair's "first"
     *          field will be field_entries.end() if no occurrence of "tag" was found.  Otherwise the pair's
     *          "second" field will point one past the last occurrence of "tag".
     */
    static std::pair<std::vector<DirectoryEntry>::const_iterator, std::vector<DirectoryEntry>::const_iterator>
        FindFields(const std::string &tag, const std::vector<DirectoryEntry> &field_entries);
};


#endif // ifndef DIRECTORY_ENTRY_H
