#include <Arduino.h>

#define LED_PIN  8  //  Pin number for the LED

void setup()
{
    // initialize digital pin LED_PIN as an output.
    pinMode(LED_PIN, OUTPUT);
}

// the loop function runs over and over again forever
void loop()
{
    digitalWrite(LED_PIN, HIGH);  // change state of the LED by setting the pin to the HIGH voltage level
    delay(1000);                      // wait for a second
    digitalWrite(LED_PIN, LOW);   // change state of the LED by setting the pin to the LOW voltage level
    delay(1000);                      // wait for a second
}
