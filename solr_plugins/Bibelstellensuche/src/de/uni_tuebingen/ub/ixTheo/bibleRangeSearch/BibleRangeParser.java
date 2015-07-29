package de.uni_tuebingen.ub.ixTheo.bibleRangeSearch;

import org.apache.lucene.search.FilteredQuery;
import org.apache.lucene.search.Query;
import org.apache.solr.common.params.SolrParams;
import org.apache.solr.request.SolrQueryRequest;
import org.apache.solr.search.QParser;
import org.apache.solr.search.SyntaxError;

public class BibleRangeParser extends QParser {
    private final static String QUERY_SEPARATOR = " ";
    private final static String DB_FIELD_SEPARATOR = ",";

    private Query innerQuery;

    public BibleRangeParser(final String searchString, final SolrParams localParams, final SolrParams params, final SolrQueryRequest request) {
        super(searchString, localParams, params, request);
        try {
            final String queryString = "+bible_ranges:" + getBookPrefixQueryString(searchString);
            final QParser parser = getParser(queryString, "lucene", getReq());
            final Range[] ranges = getRangesFromQuery();
            this.innerQuery = new FilteredQuery(new BibleRangeQuery(parser.parse(), ranges), new BibleRangeFilter(ranges));
        } catch (SyntaxError ex) {
            throw new RuntimeException("error parsing query", ex);
        }
    }

    /**
     * Tries to extract the book index of a search query.
     * Then creates a query string by concatenating the book index with '*'.
     * If no book index is found, only '*' will be returned.
     *
     * @param queryString The search string from user
     * @return e.g. "12*" or "*"
     */
    private String getBookPrefixQueryString(final String queryString) {
        if (queryString == null || queryString.length() < 2) {
            return "*";
        }
        return queryString.substring(0, 2) + "*";
    }

    @Override
    public Query parse() throws SyntaxError {
        return this.innerQuery;
    }

    private Range[] getRangesFromQuery() {
        return BibleRange.getRanges(getFieldsFromQuery());
    }

    private String[] getFieldsFromQuery() {
        return qstr.split(QUERY_SEPARATOR);
    }

    public static Range[] getRangesFromDatabaseField(final String db_field) {
        return BibleRange.getRanges(db_field, DB_FIELD_SEPARATOR);
    }
}
