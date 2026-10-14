package java.sql;

/**
 * Minimal Timestamp with millisecond precision, nanoseconds are not tracked separately
 */
public class Timestamp extends java.util.Date {

    public Timestamp(long time) {
        super(time);
    }

    public int getNanos() {
        return (int) ((getTime() % 1000 + 1000) % 1000) * 1000000;
    }

    public void setNanos(int nanos) {
        setTime(getTime() / 1000 * 1000 + nanos / 1000000);
    }
}
