package org.slashengine.services;

import android.app.Activity;

/**
 * An analytics backend written in Java, over an analytics service's SDK. Events only arrive
 * while the player consents, so it needs nothing from the game but its calls.
 */
public interface AnalyticsBackend {
    /** A short name for services.state and the log, e.g. "firebase". */
    String name();

    /** Called once before anything else. */
    default void start(Activity activity, Reports reports) {}

    /** The player's consent, each "unknown", "granted" or "denied". */
    default void setConsent(String analytics, String personalizedAds) {}

    /** An event, with its parameters as a JSON object of strings, numbers and booleans. */
    void logEvent(String name, String paramsJson);

    default void setUserProperty(String name, String value) {}

    default void setUserId(String id) {}

    /** Sends what is batched now, as when the game goes to the background. */
    default void flush() {}
}
