package java.sql;

/**
 * Minimal SQL date, a thin wrapper over java.util.Date
 */
public class Date extends java.util.Date {

    public Date(long date) {
        super(date);
    }
}
