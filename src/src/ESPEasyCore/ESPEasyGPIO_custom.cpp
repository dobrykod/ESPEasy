#include "../ESPEasyCore/ESPEasyGPIO.h"

/****************************************************************************/
// Central functions for GPIO handling
// **************************************************************************/
#include "../../_Plugin_Helper.h"

void log(String text)
{
  if (loglevelActiveFor(LOG_LEVEL_INFO))
    addLogMove(LOG_LEVEL_INFO, text);
}

void log(String pinName, int8_t state)
{
  log(pinName + '=' + state);
}

typedef std::map<uint8_t, String> PinNamesMap;
PinNamesMap pinNames;

#ifdef USES_P009

// The status of all pins of all MCP chips is checked periodically
// We store the latest reading and the previous one
_Alignas(uint64_t) uint16_t MCP_Pin_States[2][8];
// This points to place where the latest reading is stored
int MCP_Pin_States_Latest_Index = 0;
int MCP_Pin_States_Previous_Index() { return (MCP_Pin_States_Latest_Index + 1) % 2; }

int8_t MCP_Get(uint8_t port, int arrayIndex)
{
  int unit = (port - 1) / 16;
  int pin  = (port - 1) % 16;
  return (MCP_Pin_States[arrayIndex % 2][unit % 8] >> pin) & 1;
}

int8_t MCP_Get_Latest(uint16_t port)
{
  return MCP_Get(port, MCP_Pin_States_Latest_Index);
}

int8_t MCP_Get_Previous(uint16_t port)
{
  return MCP_Get(port, MCP_Pin_States_Latest_Index + 1);
}

// Checks for changes on all MCP pins and generates events for them
void MCP_Send_All_Events()
{
  const int latest_index = MCP_Pin_States_Latest_Index;
  const int previous_index = MCP_Pin_States_Previous_Index();

  // Iterate all chips
  for (int unit = 0; unit < 8; ++unit)
  {
    // Most of the time inputs do not change
    // Take closer look only if status of any pin changes
    uint16_t latest = MCP_Pin_States[latest_index][unit];
    uint16_t previous = MCP_Pin_States[previous_index][unit];
    uint16_t diff = latest ^ previous;
    if(diff)
    {
      for (int pin = 0; pin < 16; ++pin)
      {
        if((diff >> pin) & 1)
        {
          uint16_t port = unit * 16 + pin + 1;
          auto it_name = pinNames.find(port);
          if(it_name != pinNames.end())
            log(it_name->second, (latest >> pin) & 1);
        }
      }
    }
  }
}

// Reads and stores MCP pins states
void MCP_Read_All_Pins()
{
  // Change Latest_Index to point on the old data, we are going to override it
  MCP_Pin_States_Latest_Index = MCP_Pin_States_Previous_Index();
  // Latest data as uint8 array
  uint8_t * MCP_Pin_States_Latest = (uint8_t*)MCP_Pin_States[MCP_Pin_States_Latest_Index];

  // Read all of 8 chips
  for (int unit = 0; unit < 8; ++unit)
  {
    // Read all pins of given chip and Update MCP_Pin_States only on successful read
    uint8_t address = 0x20 + unit;
    uint8_t retValue;
    if(GPIO_MCP_ReadRegister(address, MCP23017_GPIOA, &retValue))
      MCP_Pin_States_Latest[unit * 2] = retValue;

    if(GPIO_MCP_ReadRegister(address, MCP23017_GPIOB, &retValue))
      MCP_Pin_States_Latest[unit * 2 + 1] = retValue;
  }
}

struct OutputType
{
  OutputType(String name, uint8_t pin) :
    name(name),
    pin(pin)
  {
    pinNames[pin] = name;
  }

  bool Pulse_Milliseconds(int duration_ms)
  {
    // Disable timer if it set already
    Scheduler.clearGPIOTimer(PLUGIN_MCP, pin);

    bool ok = GPIO_MCP_Write(pin, 1);
    Scheduler.setGPIOTimer(duration_ms, PLUGIN_MCP, pin, 0);
    return ok;
  }

   bool Write(int8_t state)
  {
    // Disable timer if it set already
    Scheduler.clearGPIOTimer(PLUGIN_MCP, pin);
    return GPIO_MCP_Write(pin, state);
  }

  bool Write(int8_t state, int delay_seconds)
  {
    if(delay > 0)
    {
      Scheduler.setGPIOTimer(delay_seconds * 1000, PLUGIN_MCP, pin, state);
      return true;
    }
    else
    {
      return Write(state);
    }
  }

  void Toogle()
  {
    Write(MCP_Get_Latest(pin) == 0 ? 1 : 0);
  }

  void Blink(int duration_ms = 300)
  {
    auto latest = MCP_Get_Latest(pin);
    GPIO_MCP_Write(pin, !latest);
    Scheduler.setGPIOTimer(duration_ms, PLUGIN_MCP, pin, latest);
  }

  String name;
  uint8_t pin;
};

struct InputType;
std::list<InputType*> inputs;

struct InputType
{ 
  InputType(String name, uint8_t pin) :
    name(name),
    pin(pin)
  {
    pinNames[pin] = name;
    inputs.push_back(this);

    // // For details see gpio_mode_range_helper:

    // setMCPInputAndPullupMode(pin, true);

    // const __FlashStringHelper * logPrefix  = F("MCP");
    // const __FlashStringHelper * logPostfix = F("INPUT PULLUP");

    // pluginID_t pluginID  = PLUGIN_MCP;
    // const uint32_t key = createKey(pluginID, pin);
    // uint8_t mode = 255;

    // if (globalMapPortStatus[key].mode != PIN_MODE_OFFLINE)
    // {
    //   int8_t currentState;
    //   GPIO_Read(pluginID, pin, currentState);

    //   // state = currentState;

    //   if (currentState == -1) {
    //     mode = PIN_MODE_OFFLINE;

    //     // state = -1;
    //   }

    //   createAndSetPortStatus_Mode_State(key, mode, currentState);

    //   String log = logPrefix;
    //   log += strformat(F(" : port#%d: MODE set to "), pin);
    //   log += logPostfix;
    //   log += concat(F(". Value = "), currentState);
    //   addLog(LOG_LEVEL_INFO, log);
    //   SendStatusOnlyIfNeeded(event, SEARCH_PIN_STATE, key, log, 0);
    // }
  }

  bool Is_High()
  {
    return MCP_Get_Latest(pin) == 1;
  }

  bool Is_Low()
  {
    return MCP_Get_Latest(pin) == 0;
  }

  bool Is_Changed()
  {
    return MCP_Get_Latest(pin) != MCP_Get_Previous(pin);
  }

  bool Gets_High()
  {
    return MCP_Get_Latest(pin) == 1 && MCP_Get_Previous(pin) == 0;
  }

  bool Gets_Low()
  {
    return MCP_Get_Latest(pin) == 0 && MCP_Get_Previous(pin) == 1;
  }

  virtual void update()
  {
    if(Is_Low())
    {
      if(Gets_Low())
      {
        MCP_Pin_Prev_Press_Duration = MCP_Pin_Press_Duration;
        MCP_Pin_Press_Duration = 1;
      }
      else if(MCP_Pin_Press_Duration < UINT8_MAX)
      {
        MCP_Pin_Press_Duration += 1;
      }
    }
    else
    {
      if(Gets_High())
      {
        MCP_Pin_Prev_Release_Duration = MCP_Pin_Release_Duration;
        MCP_Pin_Release_Duration = 1;
      }
      else if(MCP_Pin_Release_Duration < UINT8_MAX)
      {
        MCP_Pin_Release_Duration += 1;
      }
    }
  }

  String name;
  uint8_t pin;

  // Number of consecutive calls to function "update" when the pin was in pressed / released state
  uint8_t MCP_Pin_Press_Duration = 0;
  uint8_t MCP_Pin_Release_Duration = 0;
  uint8_t MCP_Pin_Prev_Press_Duration = 0;
  uint8_t MCP_Pin_Prev_Release_Duration = 0;
};

struct ButtonType : public InputType
{
  ButtonType(String name, uint8_t pin) :
    InputType(name, pin)
  {
  }

  // If the button is pressed
  bool Is_Pressed() { return Is_Low(); }

  // If the button is released
  bool Is_Released() { return Is_High(); }

  // If the button gets pressed
  bool Gets_Pressed() { return Gets_Low(); }

  // If the button gets released
  bool Gets_Released() { return Gets_High(); }

  uint8_t Press_Duration()
  {
    if(Is_Pressed())
      return MCP_Pin_Press_Duration;
    else
      return 0;
  }

  // Call it on releasing the button to check if button was double clicked
  bool Was_Double_Click()
  {
    // short press, short release, short press
    return
      MCP_Pin_Prev_Press_Duration > 0 && MCP_Pin_Prev_Press_Duration < 6 &&
      MCP_Pin_Prev_Release_Duration > 0 && MCP_Pin_Prev_Release_Duration < 6 &&
      MCP_Pin_Press_Duration > 0 && MCP_Pin_Press_Duration < 6;
  }

  // Call it on releasing the button to check if button was long clicked
  bool Was_Long_Click(uint16_t min_duration_millis = 2500)
  {
    return MCP_Pin_Press_Duration > min_duration_millis / 100;
  }
};

struct MorseButtonType : public ButtonType
{
  MorseButtonType(String name, uint8_t pin) :
    ButtonType(name, pin)
  {
  }

  virtual void update() override
  {
    InputType::update();

    if(Gets_Released())
    {
      press_durations.push_back(MCP_Pin_Press_Duration);
      if(press_durations.size() > max_length)
        press_durations.pop_front();
    }
    else if(Is_Released() && MCP_Pin_Release_Duration > max_idle_time)
    {
      // clear it if released for too long
      print();
      press_durations.clear();
    }
  }

  void print()
  {
    if(press_durations.size() > 0)
    {
      String text;

      for(auto x : press_durations)
      {
        if(x > max_dot_time)
          text += "-";
        else
          text += ".";
        text += int(x);
      }

      log("Morse: " + text);
    }
  }

  bool check(const std::vector<uint8_t> pattern)
  {
    // check if we have expected input size
    if(press_durations.size() != pattern.size())
      return false;

    // start comparing from the end, ignore preceding input
    auto it_duration = press_durations.rbegin();
    auto it_pattern = pattern.rbegin();

    while(it_pattern != pattern.rend())
    {
      if( (*it_pattern > 0) != (*it_duration > max_dot_time) )
        return false;

      it_duration++;
      it_pattern++;
    }

    return true;
  }

  const uint8_t max_idle_time = 30; // in deciseconds
  const uint8_t max_dot_time  =  6; // in deciseconds
  uint8_t max_length = 10;
  std::list<uint8_t> press_durations;
};

struct DoorType : public InputType
{
  DoorType(String name, uint8_t pin) :
    InputType(name, pin)
  {
  }

  // Is door open
  bool Is_Open() { return Is_High(); }

  // Is door closed
  bool Is_Closed() { return Is_Low(); }

  // If the door gets open
  bool Gets_Open() { return Gets_High(); }

  // If the door gets closed
  bool Gets_Closed() { return Gets_Low(); }
};

struct RollerShutter;
std::list<RollerShutter*> shutters;

struct RollerShutter
{ 
  RollerShutter(String name, uint8_t motor_up_pin, uint8_t motor_down_pin, uint8_t button_up_pin, uint8_t button_down_pin) :
    name       (name),
    motor_up   (name + "_Motor_Up",    motor_up_pin),
    motor_down (name + "_Motor_Down",  motor_down_pin),
    button_up  (name + "_Button_Up",   button_up_pin),
    button_down(name + "_Button_Down", button_down_pin)
  {
    shutters.push_back(this);
  }

  bool Move_Up(int duration = 60)
  {
    // Assure only one motor is on at a time to avoid entering shutters config mode
    if( !motor_down.Write(0) )
      return false;

    if( !motor_up.Write(1) )
      return false;

    // Schedule disabling motor
    if(duration > 0)
      return motor_up.Write(0, duration);
    else
      return true;
  }

  bool Move_Down(int duration = 60)
  {
    // Assure only one motor is on at a time to avoid entering shutters config mode
    if( !motor_up.Write(0) )
      return false;

    if( !motor_down.Write(1) )
      return false;

    // Schedule disabling motor
    if(duration > 0)
      return motor_down.Write(0, duration);
    else
      return true;
  }

  void Stop()
  {
    motor_down.Write(0);
    motor_up.Write(0);
  }

  void Event(String args)
  {
    if(args == "config")
    {
      motor_down.Write(1);
      motor_down.Write(0, 6);
      motor_up.Write(1);
      motor_up.Write(0, 6);
    }
    else if(args == "stop")
    {
      Stop();
    }
    else if(args.startsWith("down"))
    {
      auto time = args.substring(5);
      if(time.length())
        Move_Down(time.toInt());
      else
        Move_Down();
    }
    else if(args.startsWith("up"))
    {
      auto time = args.substring(3);
      if(time.length())
        Move_Up(time.toInt());
      else
        Move_Up();
    }
  }

  String name;
  OutputType motor_up;
  OutputType motor_down;
  ButtonType button_up;
  ButtonType button_down;
};

// MCP GPIO port assignements
// Pins 33-48 are damaged.
// Pins 49-64 are damaged.
// Pins 81-96 are damaged.

const uint16_t Front_LED_Pin = 0;
const uint16_t Wiata_Przycisk_Pin = 0;

const uint16_t Poddasze_LED_Pin = 1;
const uint16_t Kotlownia_LED_Pin = 2;
const uint16_t Garaz_LED_1_Pin = 5;
const uint16_t Schody_LED_Pin = 4;
const uint16_t Garaz_LED_2_Pin = 3;
const uint16_t Korytarz_LED_Pin = 6;
const uint16_t Wiatrolap_LED_Pin = 7;
const uint16_t Podjazd_LED_Pin = 9;
const uint16_t Garaz_Wentylator_Pin = 10;
const uint16_t Wiata_LED_Pin = 11;
const uint16_t Dzwonek_Pin = 12;
const uint16_t Front_Drzwi_Pin = 29;
const uint16_t Garaz_Brama_Pin = 31;
const uint16_t Garaz_Brama_Sec_Pin = 32;
const uint16_t Garaz_Przycisk_Lewy_Pin = 65;
const uint16_t Poddasze_Przycisk_1_Pin = 66;
const uint16_t Garaz_Drzwi_Pin = 67;
const uint16_t Kotlownia_Przycisk_Pin = 68;
const uint16_t Wiatrolap_Drzwi_Pin = 69;
const uint16_t Kotlownia_Drzwi_Pin = 70;
const uint16_t Wiatrolap_Przycisk_1_Pin = 72; // Lewy górny
const uint16_t Wiatrolap_Przycisk_5_Pin = 73; // Dolny
const uint16_t Schody_Przycisk_Prawy_Parter_Pin = 75;
const uint16_t Schody_Przycisk_Lewy_Poddasze_Pin = 76;
const uint16_t Schody_Przycisk_Lewy_Parter_Pin = 77;
const uint16_t Schody_Przycisk_Prawy_Poddasze_Pin = 78;
const uint16_t Kominek_Przycisk_Lewy_Pin = 79;
const uint16_t Kominek_Przycisk_Prawy_Pin = 80;
const uint16_t Poddasze_Przycisk_2_Pin = 97;
const uint16_t Garaz_Przycisk_Prawy_Pin = 98;
const uint16_t Furtka_Przycisk_Pin = 99;
const uint16_t Wiatrolap_Przycisk_4_Pin = 103; // Prawy środkowy
const uint16_t Wiatrolap_Przycisk_2_Pin = 104; // Prawy górny
const uint16_t Wiatrolap_Przycisk_3_Pin = 105; // Lewy środkowy
const uint16_t Wiata_Drzwi_Pin = 108;
const uint16_t Spare_1_Pin = 117;
const uint16_t Spare_2_Pin = 118;
const uint16_t Furtka_Zamek_Pin = 119;
const uint16_t Furtka_LED_Pin = 120;

const std::vector<uint8_t> Furtka_Code = {0,0,1,1,0,1};

#define CONCAT_(a, b) a##b
#define CONCAT(a, b) CONCAT_(a, b)
#define CREATE_OUTPUT(name) OutputType name(#name, CONCAT(name, _Pin))
#define CREATE_BUTTON(name) ButtonType name(#name, CONCAT(name, _Pin))
#define CREATE_MORSE(name) MorseButtonType name(#name, CONCAT(name, _Pin))
#define CREATE_DOOR(name) DoorType name(#name, CONCAT(name, _Pin))
#define CREATE_SHUTTER(name, motor_up, motor_down, button_up, button_down) RollerShutter CONCAT(, name)(#name, motor_up, motor_down, button_up, button_down)

CREATE_OUTPUT(Spare_1);
CREATE_OUTPUT(Spare_2);
CREATE_OUTPUT(Furtka_LED);
CREATE_OUTPUT(Furtka_Zamek);
CREATE_OUTPUT(Dzwonek);
CREATE_OUTPUT(Podjazd_LED);
CREATE_OUTPUT(Front_LED);
CREATE_OUTPUT(Wiatrolap_LED);
CREATE_OUTPUT(Kotlownia_LED);
CREATE_OUTPUT(Garaz_Wentylator);
CREATE_OUTPUT(Garaz_Brama);
CREATE_OUTPUT(Garaz_Brama_Sec);
CREATE_OUTPUT(Garaz_LED_1);
CREATE_OUTPUT(Garaz_LED_2);
CREATE_OUTPUT(Wiata_LED);
CREATE_OUTPUT(Korytarz_LED);
CREATE_OUTPUT(Schody_LED);
CREATE_OUTPUT(Poddasze_LED);

CREATE_MORSE (Furtka_Przycisk);
CREATE_BUTTON(Wiatrolap_Przycisk_1);
CREATE_BUTTON(Wiatrolap_Przycisk_2);
CREATE_BUTTON(Wiatrolap_Przycisk_3);
CREATE_BUTTON(Wiatrolap_Przycisk_4);
CREATE_BUTTON(Wiatrolap_Przycisk_5);
CREATE_BUTTON(Kotlownia_Przycisk);
CREATE_BUTTON(Garaz_Przycisk_Lewy);
CREATE_BUTTON(Garaz_Przycisk_Prawy);
CREATE_BUTTON(Wiata_Przycisk);
CREATE_BUTTON(Kominek_Przycisk_Lewy);
CREATE_BUTTON(Kominek_Przycisk_Prawy);
CREATE_MORSE (Schody_Przycisk_Lewy_Parter);
CREATE_BUTTON(Schody_Przycisk_Prawy_Parter);
CREATE_BUTTON(Schody_Przycisk_Lewy_Poddasze);
CREATE_BUTTON(Schody_Przycisk_Prawy_Poddasze);
CREATE_BUTTON(Poddasze_Przycisk_1);
CREATE_BUTTON(Poddasze_Przycisk_2);

CREATE_DOOR(Front_Drzwi);
CREATE_DOOR(Wiatrolap_Drzwi);
CREATE_DOOR(Kotlownia_Drzwi);
CREATE_DOOR(Garaz_Drzwi);
CREATE_DOOR(Wiata_Drzwi);

CREATE_SHUTTER(Kuchnia_Roleta_N, 109, 110, 17, 18);
CREATE_SHUTTER(Kuchnia_Roleta_E, 126, 125, 19, 20);
CREATE_SHUTTER(Poddasze_Roleta_S,124, 123, 28, 27);
CREATE_SHUTTER(Poddasze_Roleta_N,121, 122, 26, 25);
CREATE_SHUTTER(Salon_Roleta,     114, 113, 23, 24);
CREATE_SHUTTER(Jadalnia_Roleta,  112, 111, 21, 22);

bool muted = false;

void ring(String args = "")
{
  if(muted == false)
    Dzwonek.Pulse_Milliseconds(150);
}

void furtka(String args = "")
{
  Furtka_Zamek.Pulse_Milliseconds(10);
  Wiatrolap_LED.Blink();
}

void brama_gar(String args = "")
{
  // Emulate clicking button
  Garaz_Brama.Pulse_Milliseconds(200);
}

void osw_wylacz(String args = "")
{
  Front_LED.Write        (0);
  Wiata_LED.Write        (0);
  Garaz_LED_1.Write      (0);
  Garaz_LED_2.Write      (0);
  Wiatrolap_LED.Write    (0);
  Kotlownia_LED.Write    (0);
  Korytarz_LED.Write     (0);
  Schody_LED.Write       (0);
  Poddasze_LED.Write     (0);
  Garaz_Wentylator.Write (0);
}

void parter_up(String args = "")
{
  Salon_Roleta.Move_Up();
  Jadalnia_Roleta.Move_Up();
  Kuchnia_Roleta_N.Move_Up();
  Kuchnia_Roleta_E.Move_Up();
}

void parter_down(String args = "")
{
  Salon_Roleta.Move_Down();
  Jadalnia_Roleta.Move_Down();
  Kuchnia_Roleta_N.Move_Down();
  Kuchnia_Roleta_E.Move_Down();
}

std::map<String, std::function<void(String)>> event_callbacks =
{
  // can be used as HTML command ip_address/control?cmd=event,xxx
  {"osw_podjazd",    [&](String args) { Podjazd_LED.Toogle();          }},

  {"wentylator_gar", [&](String args) { Garaz_Wentylator.Toogle();     }},
  {"osw_wiata",      [&](String args) { Wiata_LED.Toogle();            }},
  {"osw_gar",        [&](String args) { Garaz_LED_1.Toogle();          }},
  {"osw_gar_2",      [&](String args) { Garaz_LED_2.Toogle();          }},
  {"osw_wiatrolap",  [&](String args) { Wiatrolap_LED.Toogle();        }},
  {"osw_kot",        [&](String args) { Kotlownia_LED.Toogle();        }},
  {"osw_korytarz",   [&](String args) { Korytarz_LED.Toogle();         }},
  {"osw_schody",     [&](String args) { Schody_LED.Toogle();           }},
  {"osw_pietro",     [&](String args) { Poddasze_LED.Toogle();         }},
  {"osw_wylacz",     osw_wylacz                                         },

  {"salon",          [&](String args) { Salon_Roleta.Event(args);      }},
  {"jadalnia",       [&](String args) { Jadalnia_Roleta.Event(args);   }},
  {"kuchnia_n",      [&](String args) { Kuchnia_Roleta_N.Event(args);  }},
  {"kuchnia_e",      [&](String args) { Kuchnia_Roleta_E.Event(args);  }},
  {"poddasze_n",     [&](String args) { Poddasze_Roleta_N.Event(args); }},
  {"poddasze_s",     [&](String args) { Poddasze_Roleta_S.Event(args); }},
  {"parter_up",      parter_up                                          },
  {"parter_down",    parter_down                                        },

  {"mute",           [&](String args) { muted = true;                  }},
  {"unmute",         [&](String args) { muted = false;                 }},
  {"ring",           ring                                               },

  {"furtka",         furtka                                             },
  {"brama_gar",      brama_gar                                          }
};

// It is used to decide about using lights.
void Calc_Nightlight(bool & nightlight, bool & changed)
{
  // minutes since midnight
  int m_sunrise = node_time.sunRise.tm_hour  * 60 + node_time.sunRise.tm_min;
  int m_sunset  = node_time.sunSet.tm_hour   * 60 + node_time.sunSet.tm_min;
  int m_now     = node_time.local_tm.tm_hour * 60 + node_time.local_tm.tm_min;

  // Offset in minutes is used to turn lights on sooner and turn them off later
  int m_offset = 60;

  bool new_value = (m_now < m_sunrise + m_offset) || (m_sunset - m_offset < m_now);

  changed = (nightlight != new_value);
  nightlight = new_value;
}
#endif

// Check all pins for my home here instead of doing it only via rule files
void GPIO_Monitor10xSec_MyHome()
{
#ifdef USES_P009
  static bool nightlight = true;
  bool nightlight_changed;
  Calc_Nightlight(nightlight, nightlight_changed);

  static bool initialize = true;
  if(initialize)
  {
    initialize = false;
    // To trigger events related to nightlight changes
    nightlight_changed = true;
    // Extra call to MCP_Read_All_Pins to initialize states at MCP_Pin_States_Previous_Index
    MCP_Read_All_Pins();

    Spare_1.Write          (0);
    Spare_2.Write          (0);
    Podjazd_LED.Write      (1);
    Garaz_Brama_Sec.Write  (1);
    Garaz_Wentylator.Write (0);
    Front_LED.Write        (0);
    Furtka_Zamek.Write     (0);
    Dzwonek.Write          (0);

    for(auto shutter : shutters)
      shutter->Stop();
  }

  MCP_Read_All_Pins();

  for (auto input : inputs)
    input->update();

  MCP_Send_All_Events();

  if(nightlight_changed)
  {
    if(nightlight == false)
      osw_wylacz();
  }

  if(nightlight)
  {
    if (Wiatrolap_Drzwi.Is_Changed() || Front_Drzwi.Is_Changed() || Kotlownia_Drzwi.Is_Changed())
    {
      if(Wiatrolap_Drzwi.Is_Closed() && Front_Drzwi.Is_Closed() && Kotlownia_Drzwi.Is_Closed())
        Wiatrolap_LED.Write(0, 300);
      else if(Wiatrolap_Drzwi.Gets_Open() || Front_Drzwi.Gets_Open() || Kotlownia_Drzwi.Gets_Open())
        Wiatrolap_LED.Write(1);
    }

    if (Kotlownia_Drzwi.Is_Changed() || Garaz_Drzwi.Is_Changed())
    {
      if(Kotlownia_Drzwi.Is_Closed() && Garaz_Drzwi.Is_Closed())
        Kotlownia_LED.Write(0, 120);
      else if(Kotlownia_Drzwi.Gets_Open() || Garaz_Drzwi.Gets_Open())
        Kotlownia_LED.Write(1);
    }

    if (Wiata_Drzwi.Is_Changed())
    {
      if(Wiata_Drzwi.Is_Closed())
        Wiata_LED.Write(0, 120);
      else
        Wiata_LED.Write(1);
    }

    if (Front_Drzwi.Is_Changed())
    {
      if(Front_Drzwi.Is_Closed())
        Front_LED.Write(0, 30);
      else
        Front_LED.Write(1);
    }
  }

  if (Wiata_Drzwi.Is_Changed() || Garaz_Drzwi.Is_Changed())
  {
    if(Wiata_Drzwi.Is_Closed() && Garaz_Drzwi.Is_Closed())
      Garaz_LED_1.Write(0, 120);
    else if(Wiata_Drzwi.Gets_Open() || Garaz_Drzwi.Gets_Open())
      Garaz_LED_1.Write(1);
  }

  if(Kominek_Przycisk_Lewy.Gets_Pressed())
    Korytarz_LED.Toogle();

  if(Kominek_Przycisk_Prawy.Gets_Pressed())
    Wiatrolap_LED.Toogle();

  if(Wiatrolap_Przycisk_5.Gets_Pressed())
    Wiatrolap_LED.Toogle();

  if(// Press two buttons at the same time
    (Wiatrolap_Przycisk_1.Gets_Pressed() && Wiatrolap_Przycisk_3.Is_Pressed()) ||
    (Wiatrolap_Przycisk_3.Gets_Pressed() && Wiatrolap_Przycisk_1.Is_Pressed())
    )
  {
    furtka();
  }

  if(Kotlownia_Przycisk.Gets_Pressed())
    Kotlownia_LED.Toogle();

  if(Garaz_Przycisk_Lewy.Press_Duration() == 10)
    Garaz_LED_2.Toogle();
  else if(Garaz_Przycisk_Lewy.Gets_Released() && Garaz_Przycisk_Lewy.Was_Long_Click(1000) == false)
    Garaz_LED_1.Toogle();

  if(Garaz_Przycisk_Prawy.Gets_Pressed())
    brama_gar();

  if(Wiata_Przycisk.Gets_Pressed())
    Wiata_LED.Toogle();

  if(Schody_Przycisk_Lewy_Parter.Gets_Pressed())
    Schody_LED.Toogle();

  if(Schody_Przycisk_Prawy_Parter.Gets_Pressed())
    Korytarz_LED.Toogle();

  if(Schody_Przycisk_Lewy_Poddasze.Gets_Pressed())
    Schody_LED.Toogle();

  if(Schody_Przycisk_Prawy_Poddasze.Gets_Pressed())
    Poddasze_LED.Toogle();

  if(Poddasze_Przycisk_1.Gets_Pressed() || Poddasze_Przycisk_2.Gets_Pressed())
    Poddasze_LED.Toogle();

  if(Furtka_Przycisk.Gets_Pressed())
  {
    Furtka_LED.Pulse_Milliseconds(200);
    if(Furtka_Przycisk.press_durations.size() == 0)
      ring();
  }
  else if(Furtka_Przycisk.Gets_Released())
  {
    if(Furtka_Przycisk.check(Furtka_Code))
      furtka();
  }

  for(auto shutter : shutters)
  {
    if(shutter->button_up.Gets_Released())
    {
      if(shutter->button_up.Was_Long_Click() || shutter->button_up.Was_Double_Click())
        shutter->Move_Up(60);
      else
        shutter->Stop();
    }
    else if(shutter->button_up.Gets_Pressed())
    {
      shutter->Move_Up();
    }
    else if(shutter->button_down.Gets_Released())
    {
      if(shutter->button_down.Was_Long_Click() || shutter->button_down.Was_Double_Click())
        shutter->Move_Down(60);
      else
        shutter->Stop();
    }
    else if(shutter->button_down.Gets_Pressed())
    {
      shutter->Move_Down();
    }
  }
#endif
}

void Event_Hook(const String& event)
{
#ifdef USES_P009
  int index = event.indexOf('=');

  String cmd;
  String arg;

  if(index > 0)
  {
    cmd = event.substring(0, index);
    arg = event.substring(index + 1);
  }
  else
  {
    cmd = event;
  }

  auto it = event_callbacks.find(cmd);
  if(it != event_callbacks.end())
    it->second(arg);
#endif
}
