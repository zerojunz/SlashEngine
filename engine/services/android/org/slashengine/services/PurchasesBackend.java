package org.slashengine.services;

import android.app.Activity;

/**
 * A purchases backend written in Java, over a store's SDK such as Google Play Billing. type is
 * "consumable", "non_consumable" or "subscription".
 */
public interface PurchasesBackend {
    /** A short name for services.state and the log, e.g. "google_play". */
    String name();

    /** Called once before anything else; keep reports for the store's callbacks. */
    void start(Activity activity, Reports reports);

    /** The player's consent, each "unknown", "granted" or "denied". */
    default void setConsent(String analytics, String personalizedAds) {}

    /**
     * Asks the store about the products, a JSON array of objects with "id" and "type": report
     * products or productsFailed, then any purchases the store has not had finished.
     */
    void fetchProducts(String catalogJson);

    /** Report the purchase's outcome. */
    void purchase(String product, String type);

    /**
     * The game granted the purchase: acknowledge it, consuming a consumable so it can be bought
     * again.
     */
    void finish(String product, String type, String transaction, String receipt);

    /** Report each owned non-consumable and subscription as "restored", then restoreFinished. */
    void restore();
}
