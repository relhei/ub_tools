#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include "BibleReferenceParser.h"
#include "MapIO.h"
#include "StringUtil.h"
#include "util.h"


void SplitIntoBookAndChaptersAndVerses(const std::string &bib_ref_candidate, std::string * const book_candidate,
				       std::string * const chapters_and_verses_candidate)
{
    book_candidate->clear();
    chapters_and_verses_candidate->clear();

    const size_t len(bib_ref_candidate.length());
    if (len <= 3)
	*book_candidate = bib_ref_candidate;
    else if (isdigit(bib_ref_candidate[len - 1])
	     or (isalpha(bib_ref_candidate[len - 1]) and isdigit(bib_ref_candidate[len - 2])))
    {
	const size_t last_space_pos(bib_ref_candidate.rfind(' '));
	if (last_space_pos == std::string::npos)
	    *book_candidate = bib_ref_candidate;
	else {
	    *book_candidate = bib_ref_candidate.substr(0, last_space_pos);
	    *chapters_and_verses_candidate = bib_ref_candidate.substr(last_space_pos + 1);
	}
    } else
	*book_candidate = bib_ref_candidate;
}


void Usage() {
    std::cerr << "usage: " << progname << " [--debug] bible_reference_candidate books_of_the_bible_to_code_map\n";
    std::cerr << "          books_of_the_bible_to_canonical_form_map pericopes_to_codes_map\n";
    std::exit(EXIT_FAILURE);
}


int main(int argc, char **argv) {
    progname = argv[0];

    bool verbose(false);

    if (argc == 6) {
	if (std::strcmp(argv[1], "--debug") != 0)
	    Usage();
	verbose = true;
	++argv, --argc;
    }

    if (argc != 5)
	Usage();

    //
    // Deal with pericopes first...
    //

    std::unordered_multimap<std::string, std::string> pericopes_to_codes_map;
    MapIO::DeserialiseMap(argv[4], &pericopes_to_codes_map);

    std::string bib_ref_candidate(StringUtil::Trim(StringUtil::ToLower(argv[1])));
    StringUtil::CollapseWhitespace(&bib_ref_candidate);

    const auto begin_end(pericopes_to_codes_map.equal_range(bib_ref_candidate));
    if (begin_end.first != begin_end.second) {
	if (verbose)
	    std::cerr << "Found a pericope to codes mapping.\n";
	for (auto pair(begin_end.first); pair != begin_end.second; ++pair)
	    std::cout << pair->second << '\n';
	return EXIT_SUCCESS;
    }

    //
    // ...now deal w/ ordinary references.
    //

    std::string book_candidate, chapters_and_verses_candidate;
    SplitIntoBookAndChaptersAndVerses(bib_ref_candidate, &book_candidate, &chapters_and_verses_candidate);
    if (verbose) {
	std::cerr << "book_candidate = \"" << book_candidate << "\"\n";
	std::cerr << "chapters_and_verses_candidate = \"" << chapters_and_verses_candidate << "\"\n";
    }

    // Map from noncanonical bible book forms to the canonical ones:
    std::unordered_map<std::string, std::string> books_of_the_bible_to_canonical_form_map;
    MapIO::DeserialiseMap(argv[3], &books_of_the_bible_to_canonical_form_map);
    const auto non_canonical_form_and_canonical_form(books_of_the_bible_to_canonical_form_map.find(book_candidate));
    if (non_canonical_form_and_canonical_form != books_of_the_bible_to_canonical_form_map.end()) {
	if (verbose)
	    std::cerr << "Replacing \"" << book_candidate << "\" with \""
		      << non_canonical_form_and_canonical_form->second << "\".\n";
	book_candidate = non_canonical_form_and_canonical_form->second;
    }

    std::unordered_map<std::string, std::string> bible_books_to_codes_map;
    MapIO::DeserialiseMap(argv[2], &bible_books_to_codes_map);
    const auto bible_book_and_code(bible_books_to_codes_map.find(book_candidate));
    if (bible_book_and_code == bible_books_to_codes_map.end()) {
	if (verbose)
	    std::cerr << "No mapping from \"" << book_candidate << "\" to a book code was found!\n";
	return EXIT_FAILURE; // Unknown bible book!
    }

    const std::string book_code(bible_book_and_code->second);
    if (verbose)
	std::cerr << "book code = \"" << book_code << "\"\n";
    if (chapters_and_verses_candidate.empty()) {
	std::cout << book_code << "00000:" << book_code << "99999" << '\n';
	return EXIT_SUCCESS;
    }

    std::set<std::pair<std::string, std::string>> start_end;
    if (not ParseBibleReference(chapters_and_verses_candidate, book_code, &start_end)) {
	if (verbose)
	    std::cerr << "The parsing of \"" << chapters_and_verses_candidate
		      << "\" as chapters and verses failed!\n";
	return EXIT_FAILURE;
    }

    for (const auto &pair : start_end)
	std::cout << pair.first << ':' << pair.second << '\n';
}
