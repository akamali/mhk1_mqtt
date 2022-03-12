# MHK1 + MQTT (Home Assistant, etc)

Control Mitsubishi Heat Pump using MHK1 and MQTT

DISCLAIMER: There is a chance that using the code here causes serious damage to your equipment, use this project at your own risk. For the most part this project is a giant hack and there hasn't been a ton of proper engineering/programming effort put into it. 

## ESP32 MQTT MHK1
### Why would you want to use this project?
Most people should try and use the original code from https://github.com/SwiCago/HeatPump. It's well tested and it is still actively maintained. That project is useful for people who are interested in having remote control over the heat pump by going through the CN105 port.

However, the project above requires full control of the heat pump and does not allow anything else to connect to the CN105 port, so you'd have to give up your MHK1. The project here is useful for people who don't want to give up their MHK1, but are still interested in ability to remotely change temperature settings of the heat pump. If you don't have an official controller (such as MHk1), or you are not interested in keeping your controller then this project is not for you.

This project allows you to both use MHK1 and home automation solutions such as Home Assistant to control the heat pump.

### Why is this a separate project?
This project is a derivative of SwiCago/HeatPump. Unfortunately the changes needed were too invasive to put into the main project, so I've made a fork here. The primary issue is that MHK1 is pretty strict about its timing and expects responses in under a second. It's also sensitive about the response codes exactly matching what it expects.

Code from SwiCago has relatively long delays that causes MHK1 to get disconnected, and the code doesn't keep track of requests and responses (since it doesn't need to), and it causes MHK1 to trip up often and get disconnected.

I believe there is a way to change enough things in SwiCago/HeatPump so that the code here can be merged in, but that required a major redo of the project and it has a high risk of breaking stuff for existing users (who don't care about MHK1).

### OK, I'm interested, where do I start?
In order to use this project you need a microcontroller that has two UARTs, such as ESP32. Currently I'm using a ESP32-C3-32S ([aliexpress](https://www.aliexpress.com/wholesale?catId=0&initiative_id=SB_20220310171133&SearchText=ESP32-C3-32S)), but any ESP32 should work.

First step is to make a connection from your ESP32 to your heat pump. To get that working please refer to the instructions from SwiCago/HeatPump. Once you get the examples working from that project you can continue here. See [HeatPumpEmulator](https://github.com/akamali/mhk1_mqtt#heat-pump-emulator) below, it can be used to test you have the right set up without necessarily connecting your microcontroller to your heat pump.

Note that in addition to the parts listed there you also need a female version of PAP-05V-S. I couldn't find one locally, so I got a generic female 5P 2.0mm connector and I broke enough pieces from it to get it to fit.

<img src="https://github.com/akamali/mhk1_mqtt/blob/master/CN105-Female.jpg"/>

You need to set up your pins in the code below:
```
heatpumpSerial.begin(2400, SERIAL_8E1, 9, 10);
mhk1.begin(2400, SERIAL_8E1, 1, 2);
```
You might also have to make changes here to change the assignment:
```
HardwareSerial& heatpumpSerial = Serial;
HardwareSerial& mhk1 = Serial1;
```

The rest is simple, connect your male and female connectors to the right pins, plug in your MHK1, connect the other side to your heat pump, double check and triple check your wiring, and flip it on!

The on board lights of the ESP32-C3-32S are helpful for debugging issues
- Blinking warm (amber) light means wifi is connecting. 
- Red light means no data is being received from MHK1.
- Blue light means no data is being received from HP.
- Green light means MHK1 is not in control and settings are being overridden from MQTT.

Blue (HP not responding)
<img src="https://github.com/akamali/mhk1_mqtt/blob/master/ESP32-Blue.jpg"/>

Green (Override flag is set)
<img src="https://github.com/akamali/mhk1_mqtt/blob/master/ESP32-Green.jpg"/>

Red (Not receiving anything from MHK1)
<img src="https://github.com/akamali/mhk1_mqtt/blob/master/ESP32-Red.jpg"/>

Normal/WiFi Connected
<img src="https://github.com/akamali/mhk1_mqtt/blob/master/ESP32-Normal.jpg"/>

I recommend using [HeatPumpEmulator](https://github.com/akamali/mhk1_mqtt#heat-pump-emulator) to test things properly and work through all the issues, then hopefully connecting to the heat pump will be effortless.

### Integration with Home Assistant
I'm no Home Assistant expert and I'm sure there are better ways of achieving what I've done here, you can use my settings to start. Set up your MQTT broker and verify things work, tons of tutorials online on this.

```
climate:
  - platform: mqtt
    name: "Mitsubishi Heatpump"
    modes:
      - "heat"
      - "dry"
      - "cool"
      - "fan_only"
      - "auto"
      - "off"
    mode_command_topic: "heatpump/set"
    mode_command_template: "{ 'mode': '{{value | upper}}'}"
    mode_state_topic: "heatpump"
    mode_state_template: "{{ value_json.mode | lower}}"
    fan_mode_state_topic: "heatpump"
    fan_mode_state_template: "{{ value_json.fan }}"
    fan_mode_command_topic: "heatpump/set"
    fan_mode_command_template: "{ 'fan': '{{value}}'}"
    fan_modes:
      - "AUTO"
      - "Quiet"
      - "1"
      - "2"
      - "3"
      - "4"
    current_temperature_topic: "heatpump/status"
    current_temperature_template: "{{value_json.roomTemperature}}"
    precision: 0.5
    temperature_command_template: "{ 'temperature': '{{value}}'}"
    temperature_command_topic: "heatpump/set"
    temperature_state_topic: "heatpump"
    temperature_state_template: "{{ value_json.temperature }}"
    aux_command_topic: "heatpump/override/set"
    aux_state_topic: "heatpump/override"
    aux_state_template: "{{ value }}"
    temp_step: 0.5
```
```
sensor:
  - platform: mqtt
    name: "RemoteHP Target Temp"
    device_class: temperature
    state_topic: "heatpump"
    value_template: "{{value_json.temperature}}"
    unit_of_measurement: '°C'

  - platform: mqtt
    name: "RemoteHP Room Temp"
    device_class: temperature
    state_topic: "heatpump/status"
    value_template: "{{value_json.roomTemperature}}"
    unit_of_measurement: '°C'

  - platform: mqtt
    name: "RemoteHP StandBy"
    state_topic: "heatpump/status"
    value_template: "{{value_json.fanMode}}"

  - platform: mqtt
    name: "RemoteHP Compressor State"
    state_topic: "heatpump/status"
    value_template: "{{value_json.compressorState}}"
```

### How to use?
The idea is that MHK1 normally drives the heat pump, but you can override it by using MQTT (over Home Assistant in this case). Out of the box you should be able to use your MHK1 like normal, and any modifications done through MHK1 should show up under your climate card:

<img src="https://github.com/akamali/mhk1_mqtt/blob/master/ClimateCard.PNG"/>

When you make any changes through MQTT, then the controller goes into the override mode. While in override mode settings from MHK1 are ignored. You can tell the controller is in override mode by checking the RGB light on the ESP32, if it's green it means it's in override mode. You can also tell if override is enabled by listening to the `heatpump/override` topic, if you use the Home Assistant integration from above that flag is connected to "Aux heat" under the climate card:

<img src="https://github.com/akamali/mhk1_mqtt/blob/master/ClimateCard-AuxHeat.PNG"/>

You can turn off the override mode by:
- Switching aux heat off from Home Assistant.
- Making two consequitive changes under 60s on the MHK1. To do this make any adjustment on MHK1, since the controller is in override mode the command from MHK1 is ignored and after 10s or so the MHK1 reverts back to the settings from Home Assistant. However, if you try to change settings again (under 60s from the first try) then the controller detects this and turns off the override mode. Now MHK1 can be used again to drive the heat pump.

When in override mode changes to the following are ignored:
- Target temperature
- Fan
- Vane/Wide Vane
- Power
- Mode (Heat/Cool/etc)

Note that MHK1 will continue to send room temperature readings to the heat pump even when in override mode. This room temperature value is what is used by the heat pump to adjust it's operation mode. 

### Setting remote room temperature
By default the heat pump uses room temperature readings from MHK1. You can override this by publishing a different temperature value to the `heatpump/remote_temp/set` topic. The temperature value has to be a valid positive float value in Celcius, for example: 23.5

This value has to be refreshed periodically, even if the temperature value doesn't change. If the controller doesn't receive an update in 5 minutes it will fallback to the readings from MHK1.

To disable the override and switch back to readings from MHK1 either don't publish anything for 5 minutes, or publish `0.0`.

Note that when the room temperature is being overriden the display on the MHK1 will continue to show its own reading. However, the heat pump will use the temperature you've set to adjust heating/cooling settings. You can verify room temperature is set correclty by monitoring the status topic `heatpump/status`: 
```
{
  "roomTemperature": 22.5,
  "operating": false,
  "compressorFrequency": 0,
  "compressorState": 0,
  "fanMode": 1
}
```

### (Installer) Functions
This library supports changing function settings through MQTT commands. Be extra careful when using this functionality. Not all functions and values are properly documented, and having incorrect set up can damage your equipment.

Topic to send commands is `heatpump/functions/command`, and responses are received on the `heatpump/functions` topic. Example:

Publish `{ "get": "true" }` to the command topic, this is what is received:
```
{
    "101": 2,
    "102": 1,
    "103": 1,
    "104": 1,
    "105": 2,
    ...
}
```
In order to set values, publish `{ "set": { "101": 2, "102": 1} }`, response: `Finished sending the set commands`.

Note that usually setting functions through MQTT resets the override flag.

### Technical
Read this to get an overview of how the code works. 

The code sits between the MHK1 and the heat pump. By default it acts as a proxy, any packet that is received is forwarded as is to the HP. At the same time packets are parsed and information is sent over MQTT.

The code has an internal state machine that is used to make adjustments to the packets going out. Normally this state machine is idle and does nothing. However, periodically or when needed based on commands from MQTT the state machine injects packets. Roughly injection works like below:
- MHK1: Get Errors
- Idle, so send to HP
- HP returns Errors, this is passed back to MHK1

Now when injecting:
- MHK1: Get Errors
- Injection, we intercept this packet and instead of sending this to the HP instead we send a custom packet
- HP returns a response for our custom packet, we internally handle it
- Instead of returning the response from HP we return a previously cached value to MHK1 to keep it happy

What this means is for packet injection to work there has to be a functional MHK1 connected. If we don't get any packets from MHK1 then the injection mechanism doesn't work.

#### Debugging
You can enable debug logs by publishing `on` to the `heatpump/debug/set` topic. Debug and info logs are published to `heatpump/debug` and `heatpump/info` topics.

## Heat Pump Emulator
See https://github.com/akamali/mitsubishi_heatpump_emulator. It's a library that emulates a heat pump, it can be used for debugging ESP32 issues without interacting directly with a real heat pump.
