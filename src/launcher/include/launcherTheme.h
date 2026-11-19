#ifndef LAUNCHER_THEME_H
#define LAUNCHER_THEME_H

class QApplication;

namespace LauncherTheme {

// Loads the launcher stylesheet. Does not change the palette.
void Initialize(QApplication& application);

// Applies the launcher look for a theme: 0 = system, 1 = dark, 2 = light.
void Apply(int theme);

} // namespace LauncherTheme

#endif // LAUNCHER_THEME_H
