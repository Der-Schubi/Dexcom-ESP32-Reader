/**
 * Functions for authentication and reading data form the dexcom transmitter.
 * 
 * Author: Max Kaiser
 * 24.03.2020
 */

#include "mbedtls/aes.h"
#include "Output.h"

/**
 * This function will authenticate with the transmitter using a handshake and the transmitter ID.
 * Return true if we are authenticated.
 */
bool authenticate()
{
    //Send AuthRequestTxMessage
    std::string authRequestTxMessage = {0x01, 0x19, 0xF3, 0x89, 0xF8, 0xB7, 0x58, 0x41, 0x33, 0x02 };                   // 10byte, first byte = opcode (fix), [1] - [8] random bytes as challenge for the transmitter to encrypt, last byte 0x02 = normal bt channel, 0x01 alternative bt channel
    AuthSendValue(authRequestTxMessage);

    //Recv AuthChallengeRXMessage
    std::string authChallengeRxMessage = AuthWaitToReceiveValue();                                                      // Wait until we received data from the notify callback.
    if ((authChallengeRxMessage.length() != 17) || (authChallengeRxMessage[0] != 0x03))
    {
        SerialPrintln(ERROR, "Error wrong length or opcode!");
        return false;
    }
    std::string tokenHash = "";
    std::string challenge = "";
    for(int i = 1; i < authChallengeRxMessage.length(); i++)                                                            // Start with 1 to skip opcode.
    {
        if(i < 9)
            tokenHash += authChallengeRxMessage[i];
        else
            challenge += authChallengeRxMessage[i];
    }
    //Here we could check if the tokenHash is the encrypted 8 bytes from the authRequestTxMessage ([1] to [8]);
    //To check if the Transmitter is a valid dexcom transmitter (because only the correct one should know the ID).

    //Send AuthChallengeTXMessage
    std::string hash = calculateHash(challenge, transmitterID);                                                         // Calculate the hash from the random 8 bytes the transmitter send us as a challenge.
    std::string authChallengeTXMessage = {0x04};                                                                        // opcode
    authChallengeTXMessage += hash;                                                                                     // in total 9 byte.
    AuthSendValue(authChallengeTXMessage);

    //Recv AuthStatusRXMessage
    std::string authStatusRXMessage = AuthWaitToReceiveValue();                                                         // Response { 0x05, 0x01 = authenticated / 0x02 = not authenticated, 0x01 = no bonding, 0x02 bonding
    if(authStatusRXMessage.length() == 3 && authStatusRXMessage[1] == 1)                                                // correct response is 0x05 0x01 0x02
    {
        SerialPrintln(DEBUG, "Authenticated!");
        bonding = authStatusRXMessage[2] != 0x01;
        return true;
    }
    else
        SerialPrintln(ERROR, "Authenticated FAILED!");
    return false;
}

/**
 * We have successfully authorized and now want to bond.
 * First enable the BLE security bonding options and then indicate the transmitter that he can now initiate a bonding. 
 * Return true if no error occurs.
 */
bool requestBond()
{
    if(bonding)
    {
        BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);                                                             // Enable security encryption.
        BLEDevice::setSecurityCallbacks(new MySecurity());

        BLESecurity *pSecurity = new BLESecurity();
        pSecurity->setKeySize();
        pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_ONLY);
        pSecurity->setCapability(ESP_IO_CAP_IO);
        pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

        SerialPrintln(DEBUG, "Sending Bond Request.");
        //Send KeepAliveTxMessage
        std::string keepAliveTxMessage = {0x06, 0x19};                                                                  // Opcode 2 byte = 0x06, 25 as hex (0x19)
        AuthSendValue(keepAliveTxMessage);
        //Send BondRequestTxMessage
        std::string bondRequestTxMessage = {0x07};                                                                      // Send bond command.
        AuthSendValue(bondRequestTxMessage);
        //Wait for bonding to finish
        SerialPrintln(DEBUG, "Waiting for bond.");
        while (bondingFinished == false);                                                                               //Barrier waits until bonding has finished, IMPORTANT to set the bondingFinished variable to sig_atomic_t OR volatile
        //Wait
        SerialPrintln(DEBUG, "Bonding finished.");
        return true;
    }
    else
    {
        SerialPrintln(DEBUG, "Transmitter does not want to bond so DONT send bond request.");
        return true;
    }
}

/**
 * Read the time information from the transmitter.
 */
bool readTimeMessage()
{
    std::string transmitterTimeTxMessage = {0x24, 0xE6, 0x64}; 
    ControlSendValue(transmitterTimeTxMessage);
    std::string transmitterTimeRxMessage = ControlWaitToReceiveValue();
    if ((transmitterTimeRxMessage.length() != 16) || transmitterTimeRxMessage[0] != 0x25)
        return false;
    
    uint8_t status = (uint8_t)transmitterTimeRxMessage[1];
    uint32_t currentTime = (uint32_t)(transmitterTimeRxMessage[2] + 
                                      transmitterTimeRxMessage[3]*0x100  + 
                                      transmitterTimeRxMessage[4]*0x10000 + 
                                      transmitterTimeRxMessage[5]*0x1000000);
    uint32_t sessionStartTime = (uint32_t)(transmitterTimeRxMessage[6] + 
                                           transmitterTimeRxMessage[7]*0x100  + 
                                           transmitterTimeRxMessage[8]*0x10000 + 
                                           transmitterTimeRxMessage[9]*0x1000000);
    SerialPrintf(DATA, "Time - Status:              %d\n", status);
    SerialPrintf(DATA, "Time - since activation:    %d (%d days, %d hours)\n", currentTime,                                  // Activation date is now() - currentTime * 1000
                                                                         currentTime / (60*60*24),                      // Days round down
                                                                         (currentTime / (60*60)) % 24);                 // Remaining hours
    SerialPrintf(DATA, "Time - since session start: %d\n", sessionStartTime);                                                // Session start = Activation date + sessionStartTime * 1000
    return true;
}

/**
 * Read the Battery values.
 */
bool readBatteryStatus()
{
    SerialPrintln(DEBUG, "Reading Battery Status.");
    std::string batteryStatusTxMessage ={0x22, 0x20, 0x04};
    ControlSendValue(batteryStatusTxMessage);
    std::string batteryStatusRxMessage = ControlWaitToReceiveValue();
    if(!(batteryStatusRxMessage.length() == 10 || batteryStatusRxMessage.length() == 12) || 
         batteryStatusRxMessage[0] != 0x23)
        return false;
    
    SerialPrintf(DATA, "Battery - Status:      %d\n", (uint8_t)batteryStatusRxMessage[1]);
    SerialPrintf(DATA, "Battery - Voltage A:   %d\n", (uint16_t)(batteryStatusRxMessage[2] + batteryStatusRxMessage[3]*0x100));
    SerialPrintf(DATA, "Battery - Voltage B:   %d\n", (uint16_t)(batteryStatusRxMessage[4] + batteryStatusRxMessage[5]*0x100));
    if(batteryStatusRxMessage.length() == 12)                                                                           // G5 or G6 Transmitter.
    {
        SerialPrintf(DATA, "Battery - Resistance:  %d\n", (uint16_t)(batteryStatusRxMessage[6] + batteryStatusRxMessage[7]*0x100));
        SerialPrintf(DATA, "Battery - Runtime:     %d\n", (uint8_t)batteryStatusRxMessage[8]);
        SerialPrintf(DATA, "Battery - Temperature: %d\n", (uint8_t)batteryStatusRxMessage[9]);
    }
    else if(batteryStatusRxMessage.length() == 10)                                                                      // G6 Plus Transmitter.
    {
        SerialPrintf(DATA, "Battery - Runtime:     %d\n", (uint8_t)batteryStatusRxMessage[6]);
        SerialPrintf(DATA, "Battery - Temperature: %d\n", (uint8_t)batteryStatusRxMessage[7]);
    }
    return true;
}

/**
 * Reads the glucose values from the transmitter.
 */
bool readGlucose()
{
    std::string glucoseTxMessageG5 = {0x30, 0x53, 0x36};                                                                // G5 = 0x30 the other 2 bytes are the CRC16 XMODEM value in twisted order
    std::string glucoseTxMessageG6 = {0x4e, 0x0a, 0xa9};                                                                // G6 = 0x4e
    if(transmitterID[0] != 8)                                                                                           // G5
        ControlSendValue(glucoseTxMessageG5);
    else                                                                                                                // G6
        ControlSendValue(glucoseTxMessageG6);

    std::string glucoseRxMessage = ControlWaitToReceiveValue();
    if (glucoseRxMessage.length() < 16 || glucoseRxMessage[0] != (transmitterID[0] != 8 ? 0x31 : 0x4f))                 // Opcode depends on G5 / G6
        return false;

    uint8_t status = (uint8_t)glucoseRxMessage[1];
    uint32_t sequence = (uint32_t)(glucoseRxMessage[2] + 
                                   glucoseRxMessage[3]*0x100  + 
                                   glucoseRxMessage[4]*0x10000 + 
                                   glucoseRxMessage[5]*0x1000000);
    uint32_t timestamp = (uint32_t)(glucoseRxMessage[6] + 
                                    glucoseRxMessage[7]*0x100  + 
                                    glucoseRxMessage[8]*0x10000 + 
                                    glucoseRxMessage[9]*0x1000000);

    uint16_t glucoseBytes = (uint16_t)(glucoseRxMessage[10] + 
                                       glucoseRxMessage[11]*0x100);
    boolean glucoseIsDisplayOnly = (glucoseBytes & 0xf000) > 0;
    uint16_t glucose = glucoseBytes & 0xfff;
    uint8_t state = (uint8_t)glucoseRxMessage[12];
    int trend = (int)glucoseRxMessage[13];
    SerialPrintf(DATA, "Glucose - Status:      %d\n", status);
    SerialPrintf(DATA, "Glucose - Sequence:    %d\n", sequence);
    SerialPrintf(DATA, "Glucose - Timestamp:   %d\n", timestamp);                                                            // Seconds since transmitter activation
    SerialPrintf(DATA, "Glucose - DisplayOnly: %s\n", (glucoseIsDisplayOnly ? "true" : "false"));
    SerialPrintf(GLUCOSE, "Glucose - Glucose:     %d\n", glucose);
    SerialPrintf(DATA, "Glucose - State:       %d\n", state);
    SerialPrintf(DATA, "Glucose - Trend:       %d\n", trend);
    return true;
}

/**
 * Reads the Sensor values like filtered / unfiltered raw data from the transmitter.
 */
bool readSensor()
{
    std::string sensorTxMessage = {0x2e, 0xac, 0xc5};
    ControlSendValue(sensorTxMessage);
    std::string sensorRxMessage = ControlWaitToReceiveValue();
    if((sensorRxMessage.length() != 16 && sensorRxMessage.length() != 8) || sensorRxMessage[0] != 0x2f)
        return false;

    uint8_t status = (uint8_t)sensorRxMessage[1];
    uint32_t timestamp = (uint32_t)(sensorRxMessage[2] + 
                                    sensorRxMessage[3]*0x100  + 
                                    sensorRxMessage[4]*0x10000 + 
                                    sensorRxMessage[5]*0x1000000);
    SerialPrintf(DATA, "Sensor - Status:     %d\n", status);
    SerialPrintf(DATA, "Sensor - Timestamp:  %d\n", timestamp);
    if (sensorRxMessage.length() > 8)
    {
        uint32_t unfiltered = (uint32_t)(sensorRxMessage[6] + 
                                        sensorRxMessage[7]*0x100  + 
                                        sensorRxMessage[8]*0x10000 + 
                                        sensorRxMessage[9]*0x1000000);
        uint32_t filtered = (uint32_t)(sensorRxMessage[10] + 
                                        sensorRxMessage[11]*0x100  + 
                                        sensorRxMessage[12]*0x10000 + 
                                        sensorRxMessage[13]*0x1000000);
        if (transmitterID[0] == 8)                                                                                      // G6 Transmitter
        {
                int g6Scale = 34;
                unfiltered *= g6Scale;
                filtered *= g6Scale;
        }
        SerialPrintf(DATA, "Sensor - Unfiltered: %d\n", unfiltered);
        SerialPrintf(DATA, "Sensor - Filtered:   %d\n", filtered);
    }

  return true;
}

/**
 * Reads out the last glucose calibration value.
 */ 
bool readLastCalibration()
{
    std::string calibrationDataTxMessage = {0x32, 0x11, 0x16};
    ControlSendValue(calibrationDataTxMessage);
    std::string calibrationDataRxMessage = ControlWaitToReceiveValue();
    if ((calibrationDataRxMessage.length() != 19 && calibrationDataRxMessage.length() != 20) || 
        (calibrationDataRxMessage[0] != 0x33)) 
    return false;


    uint16_t glucose = (uint32_t)(calibrationDataRxMessage[11] + calibrationDataRxMessage[12]*0x100);
    uint32_t timestamp = (uint32_t)(calibrationDataRxMessage[13] + 
                                    calibrationDataRxMessage[14]*0x100  + 
                                    calibrationDataRxMessage[15]*0x10000 + 
                                    calibrationDataRxMessage[16]*0x1000000);
    SerialPrintf(DATA, "Calibration - Glucose:   %d\n", glucose);
    SerialPrintf(DATA, "Calibration - Timestamp: %d\n", timestamp);

  return true;
}

/**
 * Sending command to initiate a disconnect from the transmitter.
 */
bool sendDisconnect()
{
    SerialPrintln(DEBUG, "Initiating a disconnect.");
    std::string disconnectTxMessage = {0x09}; 
    ControlSendValue(disconnectTxMessage);
    while(connected);                                                                                                   // Wait until onDisconnect callback was called and connected status flipped.
    return true;
}


/**
 * Encrypt using AES 182 ecb (Electronic Code Book Mode).
 */
std::string encrypt(std::string buffer, std::string id)
{
    mbedtls_aes_context aes;

    std::string key = "00" + id + "00" + id;                                                                            // The key (that also used the transmitter) for the encryption.
    unsigned char output[16];

    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, (const unsigned char *)key.c_str(), strlen(key.c_str()) * 8);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, (const unsigned char *)buffer.c_str(), output);
    mbedtls_aes_free(&aes);

    std::string returnVal = "";
    for (int i = 0; i < 16; i++)                                                                                        // Convert unsigned char array to string.
    {
        returnVal += output[i];
    }
    return returnVal;
}

/**
 * Calculates the Hash for the given data.
 */
std::string calculateHash(std::string data, std::string id)
{
    if (data.length() != 8)
    {
        SerialPrintln(ERROR, "cannot hash");
        return NULL;
    }

    data = data + data;                                                                                                 // Use double the data to get 16 byte
    std::string hash = encrypt(data, id);
    return hash.substr(0, 8);                                                                                           // Only use the first 8 byte of the hash (ciphertext)
}