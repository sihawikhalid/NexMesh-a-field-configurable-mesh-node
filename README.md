
# NexMesh: Field-Configurable ESP Mesh Node for BEMS
This sketch provides the firmware for ESP microcontrollers (ESP8266 and ESP32) and is mainly intended for Building Energy Management System (BEMS) applications.

### Overview

A key component in BEMS is the ability to have fine-grained monitoring of a building's environmental metrics such as room temperature, humidity, air quality, light intensity, occupancy, and so on. This is implemented using a wireless sensor network of heterogeneous sensors that gather data and send it to a central command center, typically located in the cloud.

In most cases, the mesh network topology (see Figure 1 above) is preferred for its wider coverage and ability to navigate sensing areas with many obstructions, which is common in smart buildings. This sketch is intended for nodes of a mesh composed of:

<img src="images/iot_wsn.png" alt="iot_wsn" width="400"/>

* **Router Nodes**: Sense data at a pre-defined interval and send it to the bridge node through multi-hop communication.

* **Bridge Node**: Connected to the internet through a WiFi gateway. It publishes its own data and the data received from other nodes in the mesh to an MQTT topic.

The Challenge & Solution
One of the main challenges in deploying such meshes is adaptability. There is a need for nodes that can easily adapt to changing environments (e.g., changing WiFi credentials, API support) and are easily programmable for different sensing requirements.

This work introduces NexMesh, an architecture based on the painlessMesh library that decouples the sensing component of a node from its network configuration. This allows for fast, field-configurable deployment of mesh WSNs that connect to the cloud via MQTT. This is accomplished by dividing the architecture into two components:

1. **Sensing Logic (Pre-deployment)**: Programmed once. Ideally, this is the only component where the user adds sensor-specific functionalities.

2. **Network Configuration (Deployment)**: Handles all networking configurations (Mesh SSID/password, WiFi gateway credentials, MQTT info) and is invoked during deployment.

### Usage

**1. Pre-deployment (Sensing Logic)**. During pre-deployment, the user chooses the sensors the nodes will interface with. You must add the sensor-specific Arduino libraries and logic to the specific sections of the sketch outlined below.

**A. Library & Variable Declaration.** Add global variables and objects associated with the sensors in the global variable declaration section:
```c++ 
// ----- Global Variables/Constants & Sensor Definitions ----- 
// Add global variables and constants for the sensing logic (START)

// [INSERT YOUR SENSOR VARIABLES HERE]

// Add global variables and constants for the sensing logic (END)
```
**B. Initialization** Add initialization, configuration, discovery protocols, and GPIO assignments in the setup() function:
```c++
// ----- Initialization & Device Discovery -----
// Initialize sensor variables, objects, etc. (START)

// [INSERT YOUR SENSOR SETUP CODE HERE]

// Initialize sensor variables, objects, etc. (END)
```
**C. Data Acquisition.** The reading of sensor data is handled by the getSensorData() function, which is called by a background task periodically at a field-determined interval. Add your reading algorithms here:
```c++
// ----- Data Acquisition & Sensing Logic ----- 
// Read sensor data (START)

// [INSERT SENSOR READING LOGIC HERE]

// Read sensor data (END)
```
