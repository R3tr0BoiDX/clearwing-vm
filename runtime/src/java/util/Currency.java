package java.util;

/**
 * Minimal Currency keyed by ISO 4217 code, without locale data
 */
public final class Currency {

    private static final HashMap<String, Currency> instances = new HashMap<>();

    private final String currencyCode;

    private Currency(String currencyCode) {
        this.currencyCode = currencyCode;
    }

    public static Currency getInstance(String currencyCode) {
        if (currencyCode == null)
            throw new NullPointerException();
        if (currencyCode.length() != 3)
            throw new IllegalArgumentException("Invalid currency code: " + currencyCode);
        synchronized (instances) {
            Currency currency = instances.get(currencyCode);
            if (currency == null) {
                currency = new Currency(currencyCode);
                instances.put(currencyCode, currency);
            }
            return currency;
        }
    }

    public String getCurrencyCode() {
        return currencyCode;
    }

    public String getSymbol() {
        return currencyCode;
    }

    public int getDefaultFractionDigits() {
        return 2;
    }

    @Override
    public String toString() {
        return currencyCode;
    }
}
