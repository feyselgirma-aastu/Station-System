#include <LiquidCrystal.h>

LiquidCrystal lcd(7,6,5,4,3,2);

int button = 8;
int buzzer = 9;

void setup() {
  lcd.begin(16,2);
  pinMode(button, INPUT_PULLUP);
  pinMode(buzzer, OUTPUT);

  lcd.print("Select Route");
}

void loop() {

  if(digitalRead(button) == LOW) {

    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Route 31 Bus");

    lcd.setCursor(0,1);
    lcd.print("Arrives: 5 min");

    tone(buzzer,1000);
    delay(500);
    noTone(buzzer);

    delay(3000);

    lcd.clear();
    lcd.print("Select Route");
  }

}