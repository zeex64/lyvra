#pragma once
struct App;

namespace ui {
void loadFonts(App& app);
void applyTheme();
void draw(App& app);  // builds the whole UI for this frame and sets app.videoAreaPx
}
