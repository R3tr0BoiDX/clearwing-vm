package java.io;

/**
 * Placeholder so libraries that reference the class literal (e.g. Objenesis) link.
 * Java serialization is not supported.
 */
public class ObjectStreamClass {

    private ObjectStreamClass() {
    }

    public static ObjectStreamClass lookup(Class<?> cl) {
        return null;
    }
}
