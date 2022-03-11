# mhk1_mqtt

Control Mitsubishi Heat Pump using MHK1 and MQTT

DISCLAIMER: There is a chance that using the code here causes serious damage to your equipment, use this project at your own risk.



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
In order to use this project you need a micro controller that has two UARTs, such as ESP32. Currently I'm using a ESP32-C3-32S, but any ESP32 should work.

First step is to make a connection from your ESP32 to your heat pump. To get that working please refer to the instructions from SwiCago/HeatPump. Once you get the examples working from that project you can continue here. Note that in addition to the parts listed there you also need a female version of PAP-05V-S. I couldn't find one locally, so I got a generic female 5P 2.0mm connector and I broke enough pieces from it to get it to fit.


## Heat Pump Emulator
