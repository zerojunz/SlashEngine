package org.slashengine.services;

import android.app.Activity;

/**
 * An ads backend written in Java, over an ad network's SDK. The game calls it on its own thread;
 * call the SDK on the thread it needs (activity.runOnUiThread for most) and report from any.
 * format is "banner", "interstitial" or "rewarded", and unitId the placement's ad unit ID on
 * Android ("" if it has none).
 */
public interface AdsBackend {
    /** A short name for services.state and the log, e.g. "admob". */
    String name();

    /** Called once before anything else; keep reports for the SDK's callbacks. */
    void start(Activity activity, Reports reports);

    /** The player's consent, each "unknown", "granted" or "denied". */
    default void setConsent(String analytics, String personalizedAds) {}

    /** Report adLoaded or adFailed. */
    void load(String placement, String format, String unitId);

    /**
     * Shows a loaded ad. Report adShown, then for an interstitial or rewarded ad adRewarded if
     * the player earned it and adClosed; or adFailed.
     */
    void show(String placement, String format, String unitId);

    /** Hides a banner. */
    void hide(String placement, String format, String unitId);
}
