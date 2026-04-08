#include "button.h"
#include <AceButton.h>

using namespace ace_button;

static AceButton aceButton(BUTTON_PIN);
static ButtonClickCallback clickCb = nullptr;
static ButtonLongPressCallback longPressCb = nullptr;

// AceButton event handler
static void handleEvent(AceButton* button, uint8_t eventType, uint8_t buttonState) {
    switch (eventType) {
        case AceButton::kEventClicked:
            if (clickCb) clickCb();
            break;
        case AceButton::kEventLongPressed:
            if (longPressCb) longPressCb();
            break;
    }
}

void button_init() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    ButtonConfig* config = aceButton.getButtonConfig();
    config->setEventHandler(handleEvent);
    config->setFeature(ButtonConfig::kFeatureClick);
    config->setFeature(ButtonConfig::kFeatureLongPress);
    config->setFeature(ButtonConfig::kFeatureSuppressAfterLongPress);
    config->setLongPressDelay(3000);  // 3 giây = long press → vào AP mode
}

void button_update() {
    aceButton.check();
}

void button_on_click(ButtonClickCallback cb) {
    clickCb = cb;
}

void button_on_long_press(ButtonLongPressCallback cb) {
    longPressCb = cb;
}
