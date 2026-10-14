package java.sql;

/**
 * Minimal SQL time, a thin wrapper over java.util.Date
 */
public class Time extends java.util.Date {

    public Time(long time) {
        super(time);
    }
}
