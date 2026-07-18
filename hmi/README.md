# Obsolete Legacy HMI Reference

> [!WARNING]
> **OBSOLETE — DO NOT FLASH OR USE WITH V2**
> This directory contains an archival copy of the legacy Cheap Yellow Display (CYD) Arduino sketch (`ArgusControl_HMI-Peristaltic_Pump.ino`).
> 
> It is obsolete and incompatible with the Argus V2 controller telemetry contract because:
> 1. It treats generated RPM as actual motor feedback.
> 2. It misinterprets generated step counts as RUN state.
> 3. It targets the legacy LEDC/PCNT hardware model.
> 
> **Do not flash this sketch to production hardware.**
