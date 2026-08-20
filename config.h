#ifndef CONFIG_H
#define CONFIG_H

/* Monitor configuration: -1 = All, 0 = First, 1 = Second */
#define TARGET_MONITOR -1

/* Bar dimensions */
#define BAR_HEIGHT 32

/* Colors in ARGB8888 format (Alpha, Red, Green, Blue) */
#define COLOR_BG   0xFF1E1E2E
#define COLOR_FG   0xFF89B4FA
#define COLOR_TEXT 0xFFCDD6F4

/* Font Configuration */
#define FONT_PATH "/usr/share/fonts/TTF/DejaVuSans.ttf"
#define FONT_SIZE 16.0f
#define TEXT_PADDING_X 12

/* System Tray Configuration */
#define TRAY_ICON_SIZE 20
#define TRAY_ICON_SPACING 6
#define TRAY_PADDING_X 8

#endif /* CONFIG_H */
