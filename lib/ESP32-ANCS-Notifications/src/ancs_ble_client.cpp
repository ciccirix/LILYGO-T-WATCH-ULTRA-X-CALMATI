// Based on the ANCS work of https://github.com/S-March and the CarWatch project

#include "ble_security.h"
#include "ancs_ble_client.h"
#include "ancs_notification_queue.h"

#include "BLEAddress.h"
#include "BLEDevice.h"
#include "BLEClient.h"
#include "BLEUtils.h"
#include "BLE2902.h"

#include "esp_log.h"

#include <Arduino.h> // Only for development
#include "dbg_log.h" // log a schermo (overlay IMPOSTA) — risolto da -I src nel build

static char LOG_TAG[] = "ANCSBLEClient";

// Fixed service IDs for the Apple ANCS service
const BLEUUID notificationSourceCharacteristicUUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
const BLEUUID controlPointCharacteristicUUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
const BLEUUID dataSourceCharacteristicUUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
const BLEUUID ancsServiceUUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");

static ANCSBLEClient *sharedInstance;

static void dataSourceNotifyCallback(
		BLERemoteCharacteristic *pDataSourceCharacteristic,
		uint8_t *pData,
		size_t length,
		bool isNotify)
{
	ESP_LOGD(LOG_TAG, "dataSourceNotifyCallback");
	sharedInstance->onDataSourceNotify(pDataSourceCharacteristic, pData, length, isNotify);
}

static void notificationSourceNotifyCallback(
		BLERemoteCharacteristic *pNotificationSourceCharacteristic,
		uint8_t *pData,
		size_t length,
		bool isNotify)
{
	ESP_LOGD(LOG_TAG, "notificationSourceNotifyCallback");
	sharedInstance->onNotificationSourceNotify(pNotificationSourceCharacteristic, pData, length, isNotify);
}

ANCSBLEClient::ANCSBLEClient()
		: notificationCB(nullptr), removedCB(nullptr), pControlPointCharacteristic(nullptr)
{
	assert(sharedInstance == nullptr);
	sharedInstance = this;
	notificationQueue = new ANCSNotificationQueue();
}

ANCSBLEClient::~ANCSBLEClient()
{
	sharedInstance = nullptr;
}

void ANCSBLEClient::startClientTask(void *params)
{
	ESP_LOGD(LOG_TAG, "Starting client");
	Serial.printf("[CLI] task start, heap=%u\n", (unsigned)ESP.getFreeHeap());
	const BLEAddress *address = (BLEAddress *)params;
	// Gate: l'ANCS richiede un link cifrato. Aspetta che il bonding sia completo prima di
	// setup()/subscribe, altrimenti getService(ANCS) fallisce e non arrivano notifiche.
	uint32_t t0 = millis();
	int wc = 0;
	while (!ble_auth_is_done() && (millis() - t0 < 30000)) {
		if (++wc % 10 == 0) Serial.printf("[CLI] attendo auth... flag=%d t=%lus\n",
		                                   (int)ble_auth_is_done(), (unsigned long)((millis() - t0) / 1000));
		delay(100);
	}
	Serial.printf("[CLI] auth_done=%d -> setup\n", (int)ble_auth_is_done());
	sharedInstance->setup(address);
	Serial.println("[CLI] setup() ritornato");

	ANCSNotificationQueue *queue = sharedInstance->notificationQueue;
	while (1)
	{
		if (queue->pendingNotificationExists())
		{
			Notification pending = queue->getNextPendingNotification();
			ESP_LOGD(LOG_TAG, "retriveNotificationData: %d", pending.uuid);
			sharedInstance->retrieveExtraNotificationData(pending);
		}
		delay(500);
	}
}

void ANCSBLEClient::setup(const BLEAddress *address)
{
	BLEClient *pClient = BLEDevice::createClient();
	BLEDevice::setSecurityCallbacks(new NotificationSecurityCallbacks()); // @todo memory leak?

	BLESecurity *pSecurity = new BLESecurity();
	pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
	pSecurity->setCapability(ESP_IO_CAP_IO);
	pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
	// Connect to the remote BLE Server.
	Serial.println("[CLI] pClient->connect()...");
	if (!pClient->connect(*address))
	{
		ESP_LOGW(LOG_TAG, "Failed to connect to remote BLE server.");
		Serial.println("[CLI] connect FALLITO");
		return;
	}
	Serial.println("[CLI] connect OK, cerco servizio ANCS");

	/** BEGIN ANCS SERVICE **/
	// Obtain a reference to the service we are after in the remote BLE server.
	BLERemoteService *pAncsService = pClient->getService(ancsServiceUUID);
	if (pAncsService == nullptr)
	{
		ESP_LOGW(LOG_TAG, "Failed to find service UUID ancsServiceUUID.");
		Serial.println("[CLI] ANCS service NON trovato");
		return;
	}
	Serial.println("[CLI] ANCS service trovato");
	// Discovery caratteristiche con RETRY: il GATT a volte non le ha ancora pronte
	// subito dopo getService() -> getCharacteristic ritorna null e il subscribe saltava.
	BLERemoteCharacteristic *pNotificationSourceCharacteristic = nullptr;
	BLERemoteCharacteristic *pDataSourceCharacteristic = nullptr;
	pControlPointCharacteristic = nullptr;
	for (int i = 0; i < 12; i++)
	{
		if (!pNotificationSourceCharacteristic) pNotificationSourceCharacteristic = pAncsService->getCharacteristic(notificationSourceCharacteristicUUID);
		if (!pControlPointCharacteristic)       pControlPointCharacteristic       = pAncsService->getCharacteristic(controlPointCharacteristicUUID);
		if (!pDataSourceCharacteristic)         pDataSourceCharacteristic         = pAncsService->getCharacteristic(dataSourceCharacteristicUUID);
		if (pNotificationSourceCharacteristic && pControlPointCharacteristic && pDataSourceCharacteristic) break;
		Serial.printf("[CLI] retry caratteristiche #%d (NS=%d CP=%d DS=%d)\n", i,
		              (int)(pNotificationSourceCharacteristic != nullptr),
		              (int)(pControlPointCharacteristic != nullptr),
		              (int)(pDataSourceCharacteristic != nullptr));
		delay(300);
	}
	if (!pNotificationSourceCharacteristic || !pControlPointCharacteristic || !pDataSourceCharacteristic)
	{
		Serial.println("[CLI] caratteristiche ANCS INCOMPLETE -> abort");
		return;
	}
	Serial.println("[CLI] tutte le caratteristiche ANCS trovate");

	const uint8_t v[] = {0x1, 0x0};
	pDataSourceCharacteristic->registerForNotify(dataSourceNotifyCallback);
	pDataSourceCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t *)v, 2, true);
	pNotificationSourceCharacteristic->registerForNotify(notificationSourceNotifyCallback);
	pNotificationSourceCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t *)v, 2, true);
	Serial.println("[CLI] SUBSCRIBE NS+DS completato");
}

BLEUUID ANCSBLEClient::getAncsServiceUUID()
{
	return ancsServiceUUID;
}

void ANCSBLEClient::setNotificationArrivedCallback(ble_notification_arrived_t cbNotification)
{
	notificationCB = cbNotification;
}

void ANCSBLEClient::setNotificationRemovedCallback(ble_notification_removed_t cbNotification)
{
	removedCB = cbNotification;
}

void ANCSBLEClient::retrieveExtraNotificationData(Notification &pending)
{
	uint8_t uuid[4];
	uint32_t notifyUUID = pending.uuid;
	uuid[0] = notifyUUID;
	uuid[1] = notifyUUID >> 8;
	uuid[2] = notifyUUID >> 16;
	uuid[3] = notifyUUID >> 24;

	bool notificationAlreadyInQueue = notificationQueue->contains(pending.uuid);
	if (notificationAlreadyInQueue == false)
	{
		notificationQueue->addNotification(notifyUUID, pending, isIncomingCall(pending));
	}

	const uint8_t vIdentifier[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDAppIdentifier};
	pControlPointCharacteristic->writeValue((uint8_t *)vIdentifier, 6, true);
	const uint8_t vTitle[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDTitle, 0x0, 0x10};
	pControlPointCharacteristic->writeValue((uint8_t *)vTitle, 8, true);
	const uint8_t vMessage[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDMessage, 0x0, 0x10};
	pControlPointCharacteristic->writeValue((uint8_t *)vMessage, 8, true);
	const uint8_t vDate[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDDate};
	pControlPointCharacteristic->writeValue((uint8_t *)vDate, 6, true);
}

void ANCSBLEClient::onDataSourceNotify(
		BLERemoteCharacteristic *pNotificationSourceCharacteristic,
		uint8_t *pData,
		size_t length,
		bool isNotify)
{
	std::string message;

	uint32_t messageId = pData[4];
	messageId = messageId << 8 | pData[3];
	messageId = messageId << 16 | pData[2];
	messageId = messageId << 24 | pData[1];

	for (int i = 8; i < length; i++)
	{
		message += (char)pData[i];
	}

	ESP_LOGD(LOG_TAG, "ID: %d raw message: %s type==%d", messageId, message.c_str(), pData[5]);

	Notification *notification = notificationQueue->getNotification(messageId);

	switch (pData[5])
	{
	case ANCS::NotificationAttributeIDAppIdentifier:
		notification->type = message;
		ESP_LOGD(LOG_TAG, "got type: %s", message.c_str());
		break;
	case 0x1:
		notification->title = message;
		ESP_LOGD(LOG_TAG, "got title: %s", message.c_str());
		break;
	case 0x3:
		notification->message = message;
		ESP_LOGD(LOG_TAG, "got message: %s", message.c_str());
		break;
	}
	// Inoltra UNA sola volta dopo l'attributo Message (0x3 = ultimo contenuto richiesto
	// in retrieveExtraNotificationData: AppId, Title, Message, Date). PRIMA si pretendeva
	// title E message NON vuoti -> WhatsApp con anteprime nascoste, email e molte app
	// (corpo vuoto) NON arrivavano MAI: si vedevano solo SMS e chiamate. Ora basta che ci
	// sia un titolo O un messaggio; se entrambi vuoti, fallback al nome app.
	if (pData[5] == 0x3 && notification->isComplete == false)
	{
		if (notification->title.empty() && notification->message.empty())
			notification->title = notification->type.empty() ? std::string("Notifica") : notification->type;

		if (notificationCB)
		{
			ESP_LOGI(LOG_TAG, "notify: %s - %s ", notification->title.c_str(), notification->message.c_str());
			dbg_log("[ANCS] full cat=%d '%s'", (int)notification->category, notification->title.c_str());
			const ArduinoNotification arduinoNotification = ArduinoNotification(*notification);
			notificationCB(&arduinoNotification, notification);
		}
		notification->isComplete = true;
	}
}

bool ANCSBLEClient::isIncomingCall(const Notification &notification) const
{
	// @todo detect if it is a FaceTime or call by app? Or is category sufficient?
	return notification.category == CategoryIDIncomingCall;
}

void ANCSBLEClient::onNotificationSourceNotify(
		BLERemoteCharacteristic *pNotificationSourceCharacteristic,
		uint8_t *pData,
		size_t length,
		bool isNotify)
{
	Serial.printf("[CLI] >>> NOTIFY ricevuto! len=%u ev=%d cat=%d\n", (unsigned)length, pData[0], pData[2]);
	// Log a schermo (IMPOSTA) di OGNI evento: ev 0=Added 1=Modified 2=Removed.
	// Cosi' vediamo TUTTO cio' che iOS manda (prima loggavo solo gli Added).
	dbg_log("[ANCS] ev=%d cat=%d flag=%d", pData[0], pData[2], pData[1]);
	uint32_t messageId;

	messageId = pData[7];
	messageId = messageId << 8 | pData[6];
	messageId = messageId << 16 | pData[5];
	messageId = messageId << 24 | pData[4];

	if (pData[0] == ANCS::EventIDNotificationRemoved)
	{
		ESP_LOGI(LOG_TAG, "notification removed: %d", messageId);

		Notification *notification = notificationQueue->getNotification(messageId);

		if (isIncomingCall(*notification))
		{
			notificationQueue->addNotification(messageId, *notification, false);
			notificationQueue->removeCallNotification();
		}

		if (removedCB)
		{
			const ArduinoNotification arduinoNotification = ArduinoNotification(*notification);
			removedCB(&arduinoNotification, notification);
		}
	}
	else if (pData[0] == ANCS::EventIDNotificationAdded ||
	         pData[0] == ANCS::EventIDNotificationModified)
	{
		// Gestiamo anche i MODIFIED: WhatsApp/altre app aggregano i messaggi della
		// stessa chat e mandano "Modified" invece di "Added" -> prima li ignoravamo.
		ESP_LOGI(LOG_TAG, "notification add/mod, type: %d", pData[2]);
		Notification pending;
		pending.uuid = messageId;
		pending.eventFlags = pData[1];
		pending.category = NotificationCategory(pData[2]);
		pending.categoryCount = pData[3];
		notificationQueue->addPendingNotification(pending);
	}
}

void ANCSBLEClient::performAction(uint32_t notifyUUID, uint8_t actionID)
{
	uint8_t uuid[4];
	uuid[0] = notifyUUID;
	uuid[1] = notifyUUID >> 8;
	uuid[2] = notifyUUID >> 16;
	uuid[3] = notifyUUID >> 24;

	const uint8_t vPerformAction[] = {ANCS::CommandIDPerformNotificationAction, uuid[0], uuid[1], uuid[2], uuid[3], actionID};
	pControlPointCharacteristic->writeValue((uint8_t *)vPerformAction, (sizeof(vPerformAction) / sizeof(vPerformAction[0])), true);
}
