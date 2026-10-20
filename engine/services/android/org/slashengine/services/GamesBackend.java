package org.slashengine.services;

import android.app.Activity;

/**
 * A games backend written in Java, over a games service's SDK such as Google Play Games
 * Services. The game calls it on its own thread; call the SDK on the thread it needs and report
 * from any. achievement and leaderboard are the game's names for them, and id their IDs on
 * Android ("" if they have none).
 */
public interface GamesBackend {
    /** A short name for services.state and the log, e.g. "play_games". */
    String name();

    /** Called once before anything else; keep reports for the SDK's callbacks. */
    void start(Activity activity, Reports reports);

    /** The player's consent, each "unknown", "granted" or "denied". */
    default void setConsent(String analytics, String personalizedAds) {}

    /**
     * Report signedIn or signInFailed. interactive is false when the game starts, where Play
     * Games signs a returning player in without asking (isAuthenticated), and true when the game
     * asks, where it may show its sign-in screen (signIn).
     */
    void signIn(boolean interactive);

    /**
     * After signedIn: report achievements with the player's progress, or achievementFailed with
     * a null achievement. achievementsJson is a JSON array of objects with "achievement", "id"
     * and "steps" (an incremental achievement's steps, or 0).
     */
    void loadAchievements(String achievementsJson);

    /** Report achievementFailed if the platform refuses. */
    void unlock(String achievement, String id);

    /**
     * An incremental achievement's steps done, out of total, which unlocks it when they are all
     * done, as Play Games' setSteps. Report achievementFailed if the platform refuses.
     */
    void setSteps(String achievement, String id, int steps, int total);

    /** Report scoreSubmitted or scoreFailed. */
    void submitScore(String leaderboard, String id, long score);

    /**
     * Report scores or scoresFailed for the best count scores. time is "all_time", "week" or
     * "today", and collection "global" or "friends".
     */
    void loadScores(String leaderboard, String id, String time, String collection, int count);

    /** Shows the achievements screen; report gamesUiClosed when the player closes it. */
    void showAchievements();

    /**
     * Shows a leaderboard's screen, or every leaderboard's when leaderboard is ""; report
     * gamesUiClosed.
     */
    void showLeaderboards(String leaderboard, String id);
}
