package com.example.hello;

import android.util.Log;
import org.slashengine.services.AnalyticsBackend;

/**
 * Glue code for an SDK whose API is Java: a class implementing the engine's interface, which
 * sdk.json's "backends" names and the game makes when it starts.
 */
public final class HelloAnalytics implements AnalyticsBackend {
    @Override
    public String name() {
        return "hello_java";
    }

    @Override
    public void logEvent(String name, String paramsJson) {
        Log.i("hello", "analytics event " + name + " " + paramsJson);
    }
}
