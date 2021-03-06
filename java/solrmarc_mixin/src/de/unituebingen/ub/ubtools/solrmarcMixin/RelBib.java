package de.unituebingen.ub.ubtools.solrmarcMixin;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.Reader.*;
import java.io.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.*;
import java.util.logging.Logger;
import org.marc4j.marc.DataField;
import org.marc4j.marc.Record;
import org.marc4j.marc.VariableField;
import org.marc4j.marc.*;
import org.solrmarc.index.SolrIndexerMixin;
import org.solrmarc.tools.Utils;


public class RelBib extends IxTheo {
    protected static Logger logger = Logger.getLogger(RelBib.class.getName());
    protected final static Pattern RELBIB_POSITIVE_MATCH_PATTERN =
        Pattern.compile("^A.*|^B.*|^HD.*|^HH.*|^KB.*|" +
                        "KCA|KCG|KDG|KDH|NBC|NBD|NBE|NBH|NBK|NBQ|NCB|NCC|NCD|NCE|NCF|NCG|NCH|NCJ|" +
                        "^T.*|^V.*|^X.*|^Z.*|^.*Unassigned.*");
    final static String TRUE = "true";
    final static String FALSE = "false";


    /*
     * Predicate to check whether an IxTheo-Notation is relevant for RelBib
     */

    protected Boolean isNotRelevantForRelBib(String notation) {
        Matcher matcher = RELBIB_POSITIVE_MATCH_PATTERN.matcher(notation);
        return !matcher.matches();
    }


    /*
     * Like the IxTheo analog but filter out all notations not relevant for
     * RelBib
     */

    public Set<String> getRelBibNotationFacets(final Record record) {
        // Due to problems with facet filter excludes (OR-facets disable the filter
        // facet query and display results based on the whole collection) we have to make sure
        // that only "real" RelBib-entries get a notation
        if (record.getVariableFields("REL").isEmpty())
            return new HashSet<String>();

        Set<String> relBibNotations = getIxTheoNotationFacets(record);
        Iterator<String> relBibNotationsIter = relBibNotations.iterator();
        while (relBibNotationsIter.hasNext()) {
            if (isNotRelevantForRelBib(relBibNotationsIter.next()))
                relBibNotationsIter.remove();
        }
        return relBibNotations;
    }


    public String getIsProbablyReligiousStudies(final Record record) {
        final List<VariableField> _191Fields = record.getVariableFields("191");
        for (final VariableField _191Field : _191Fields) {
            final DataField dataField = (DataField) _191Field;
            final Subfield subfieldA = dataField.getSubfield('a');
            if (subfieldA != null && subfieldA.getData().equals("1"))
                return TRUE;
        }
        return FALSE;

    }



    public String getIsReligiousStudies(final Record record) {
        final List<VariableField> _RELFields = record.getVariableFields("REL");
        return !_RELFields.isEmpty() ? TRUE : FALSE;
    }
}
