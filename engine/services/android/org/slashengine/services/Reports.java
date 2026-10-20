package org.slashengine.services;

/**
 * Where a backend reports what happened, from any thread. The game hears of each report at its
 * next tick, on its own thread and in order. Each backend gets its own Reports in start; reports
 * made after the game stopped the backend are dropped.
 */
public final class Reports {
    private final long handle;

    Reports(long handle) {
        this.handle = handle;
    }

    /** The ad is ready to show. */
    public void adLoaded(String placement) {
        report(handle, "ad_loaded", placement, null, 0.0);
    }

    /** Loading or showing the ad failed, e.g. with "no fill". */
    public void adFailed(String placement, String error) {
        report(handle, "ad_failed", placement, error, 0.0);
    }

    /** The ad is on screen. */
    public void adShown(String placement) {
        report(handle, "ad_shown", placement, null, 0.0);
    }

    /** The player watched a rewarded ad far enough to earn the network's reward. */
    public void adRewarded(String placement, String type, double amount) {
        report(handle, "ad_rewarded", placement, type, amount);
    }

    /** The player earned a rewarded ad's reward, and the network gave none: the placement's. */
    public void adRewarded(String placement) {
        report(handle, "ad_rewarded", placement, null, 0.0);
    }

    /** The ad is gone. */
    public void adClosed(String placement) {
        report(handle, "ad_closed", placement, null, 0.0);
    }

    /**
     * What the store says about the products it sells: a JSON array of objects with "id",
     * "title", "description", "price" (as the store shows it), "price_micros", "currency" (ISO
     * 4217) and "available" (default true).
     */
    public void products(String productsJson) {
        report(handle, "products", null, productsJson, 0.0);
    }

    /** The store is unavailable. */
    public void productsFailed(String error) {
        report(handle, "products_failed", null, error, 0.0);
    }

    /**
     * A purchase's outcome, or one the store delivers unasked. state is "purchased", "pending",
     * "cancelled", "failed" or "restored"; receipt is what the game's server checks with the
     * store, such as Google Play's purchase token; error says why it failed. Any may be null.
     */
    public void purchase(
            String product, String transaction, String state, String receipt, String error) {
        String json =
                "{\"product\":"
                        + quoted(product)
                        + ",\"transaction\":"
                        + quoted(transaction)
                        + ",\"state\":"
                        + quoted(state)
                        + ",\"receipt\":"
                        + quoted(receipt)
                        + ",\"error\":"
                        + quoted(error)
                        + "}";
        report(handle, "purchase", null, json, 0.0);
    }

    /** restore is done. */
    public void restoreFinished() {
        report(handle, "restore_finished", null, null, 0.0);
    }

    /** restore failed. */
    public void restoreFinished(String error) {
        report(handle, "restore_finished", null, error, 0.0);
    }

    private static String quoted(String text) {
        if (text == null) {
            return "\"\"";
        }
        StringBuilder out = new StringBuilder("\"");
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (c == '"' || c == '\\') {
                out.append('\\').append(c);
            } else if (c < 0x20) {
                out.append(String.format("\\u%04x", (int) c));
            } else {
                out.append(c);
            }
        }
        return out.append('"').toString();
    }

    private static native void report(
            long handle, String kind, String placement, String text, double amount);
}
