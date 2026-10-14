// The activity of every Android export: SDL's activity, loading the game's libmain.so and passing
// it the launch arguments. `adb shell am start -n <package>/org.slashengine.GameActivity --esa
// args --headless,--ticks,60` runs a game headless, which slash-cli export --test does.
package org.slashengine;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
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
