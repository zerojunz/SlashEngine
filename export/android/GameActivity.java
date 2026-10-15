// The activity of every Android export: SDL's activity, loading the game's libmain.so and passing
// it the launch arguments. `adb shell am start -n <package>/org.slashengine.GameActivity --esa
// args --headless,--ticks,60` runs a game headless, which slash-cli export --test does.
package org.slashengine;

import android.os.Bundle;
import android.view.Display;
import android.view.WindowManager;
import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        preferHighestRefreshRate();
    }

    // Android runs apps at 60 Hz unless they ask for more, so ask for the display's fastest mode
    // at its current resolution: 90 or 120 Hz on phones that have it. V-Sync and the frame cap
    // in project.json still decide how often the game draws.
    private void preferHighestRefreshRate() {
        Display display = getDisplay();
        if (display == null) {
            return;
        }
        Display.Mode current = display.getMode();
        Display.Mode best = current;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight()
                    && mode.getRefreshRate() > best.getRefreshRate()) {
                best = mode;
            }
        }
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(params);
    }

    @Override
    protected String[] getLibraries() {
        // SDL and the engine are linked into the game's library.
        return new String[] {"main"};
    }

    @Override
    protected String[] getArguments() {
        String[] args = getIntent().getStringArrayExtra("args");
        return args != null ? args : new String[0];
    }
}
