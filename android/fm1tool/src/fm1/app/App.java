// SPDX-License-Identifier: MPL-2.0
// Material You: on Android 12 and later the app takes its colours from the wallpaper.
package fm1.app;

import android.app.Application;

import com.google.android.material.color.DynamicColors;

public class App extends Application {
    @Override public void onCreate() {
        super.onCreate();
        DynamicColors.applyToActivitiesIfAvailable(this);
    }
}
