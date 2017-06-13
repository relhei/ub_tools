/** \file    StringDataSource.cc
 *  \brief   Declaration of class StringDataSource.
 *  \author  Dr. Johannes Ruscheinski
 */

/*
    Copyright 2017 Library of the University of Tübingen
 *
 *  This file is part of the libiViaCore package.
 *
 *  The libiViaCore package is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  libiViaCore is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with libiViaCore; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "StringDataSource.h"
#include <stdexcept>
#include <cstdio>
#include "Compiler.h"


int StringDataSource::get() {
    if (pushed_back_) {
        pushed_back_ = false;
        return pushed_back_char_;
    }

    if (unlikely(ch_ == s_.cend()))
        return EOF;
    return *ch_++;
}


void StringDataSource::putback(const char ch) {
    if (unlikely(pushed_back_))
        throw std::runtime_error("in StringDataSource::putback: can't push back two char's in a row!");
    pushed_back_char_ = ch;
    pushed_back_ = true;
}


int StringDataSource::peek() {
    if (unlikely(pushed_back_))
        return pushed_back_char_;
    if (unlikely(ch_ == s_.cend()))
        throw std::runtime_error("in StringDataSource::peek: can't peek past end-of-string!");

    return unlikely(ch_ + 1 == s_.cend()) ? EOF : *(ch_ + 1);
}