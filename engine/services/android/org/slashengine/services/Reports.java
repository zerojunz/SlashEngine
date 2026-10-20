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

    /** The player signed in: their ID on the games service and the name they show. */
    public void signedIn(String player, String name) {
        report(handle, "signed_in", player, name, 0.0);
    }

    /** The player isn't signed in. */
    public void signInFailed(String error) {
        report(handle, "sign_in_failed", null, error, 0.0);
    }

    /**
     * The player's progress after loadAchievements: a JSON array of objects with "achievement"
     * (the game's name for it), "steps" and "unlocked". Those left out have none.
     */
    public void achievements(String progressJson) {
        report(handle, "achievements", null, progressJson, 0.0);
    }

    /** The platform refused an achievement's progress, or loading it with a null achievement. */
    public void achievementFailed(String achievement, String error) {
        report(handle, "achievement_failed", achievement, error, 0.0);
    }

    /** The score is on the leaderboard. */
    public void scoreSubmitted(String leaderboard, long score) {
        report(handle, "score_submitted", leaderboard, Long.toString(score), 0.0);
    }

    /** The leaderboard refused the score. */
    public void scoreFailed(String leaderboard, String error) {
        report(handle, "score_failed", leaderboard, error, 0.0);
    }

    /**
     * The scores loadScores asked for: a JSON object with "entries", an array of objects with
     * "rank" (1 for the best), "player" (their ID), "name" and "score", best first; "player",
     * one such object for the signed-in player or null; and "total", the players with a score.
     */
    public void scores(String leaderboard, String scoresJson) {
        report(handle, "scores", leaderboard, scoresJson, 0.0);
    }

    /** Loading the scores failed. */
    public void scoresFailed(String leaderboard, String error) {
        report(handle, "scores_failed", leaderboard, error, 0.0);
    }

    /** The player closed the achievements or leaderboards screen. */
    public void gamesUiClosed() {
        report(handle, "games_ui_closed", null, null, 0.0);
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
