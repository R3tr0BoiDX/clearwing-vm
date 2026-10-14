package sun.misc;

/**
 * Stub so libraries like Objenesis link. There is no Unsafe instance ("theUnsafe" does not exist),
 * so libraries probing for it through reflection fall back to their safe code paths.
 */
public final class Unsafe {

    private Unsafe() {
    }

    public Object allocateInstance(Class<?> cls) throws InstantiationException {
        throw new InstantiationException("sun.misc.Unsafe is not supported: " + cls.getName());
    }
}
