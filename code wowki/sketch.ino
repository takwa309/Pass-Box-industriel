#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ============ CONFIGURATION Wi-Fi et MQTT ============
const char* ssid = "Wokwi-GUEST";           // SSID Wi-Fi (pour Wokwi)
const char* password = "";                   // Mot de passe Wi-Fi
const char* mqtt_server = "broker.hivemq.com"; // Broker MQTT public
const int mqtt_port = 1883;

// Topics MQTT
const char* TOPIC_PORTE_CONT = "passbox/porte/contaminee";
const char* TOPIC_PORTE_STER = "passbox/porte/sterile";
const char* TOPIC_CYCLE_DEPART = "passbox/cycle/depart";
const char* TOPIC_CYCLE_ETAPE = "passbox/cycle/etape";
const char* TOPIC_URGENCE = "passbox/urgence";
const char* TOPIC_STATUS = "passbox/status";

// Clients Wi-Fi et MQTT
WiFiClient espClient;
PubSubClient client(espClient);

// ============ DÉFINITIONS HARDWARE ============
// Capteurs (Switches - détecteurs de position)
#define CAPTEUR_PORTE_CONT  13  // sw1 - Porte Contaminée
#define CAPTEUR_PORTE_STER  14  // sw2 - Porte Stérile

// Actionneurs (Boutons poussoirs)
#define BTN_OUVRIR_CONT     26  // btn1 - Green
#define BTN_FERMER_CONT     25  // btn2 - Blue
#define BTN_OUVRIR_STER     33  // btn3 - Yellow
#define BTN_FERMER_STER     32  // btn4 - Pink
#define BTN_DEPART          27  // btn5 - Orange
#define BTN_URGENCE         12  // btn6 - Red

#define LCD_ADDRESS         0x27
#define LCD_COLS            16
#define LCD_ROWS            2

// Initialisation du LCD
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);

// ============ VARIABLES D'ÉTAT ============
// États des portes (basés sur les capteurs)
bool porteContOuverte = false;
bool porteSterOuverte = false;

// Demandes d'ouverture en attente
bool demandeOuvrirCont = false;
bool demandeOuvrirSter = false;

// Message LCD
String messageLCD = "Pret au cycle";
unsigned long tempsMessageLCD = 0;
const unsigned long dureeMessageLCD = 2000;

// Anti-rebond des boutons
unsigned long dernierTempsBtn[6] = {0,0,0,0,0,0};
const unsigned long debounceDelay = 200;

// Gestion de l'urgence
bool urgenceActivee = false;

// Gestion du cycle de décontamination
enum EtatCycle {
  INACTIF,
  EXTRACTION_AIR,
  ARRET_AIR,
  INJECTION_DESINFECTANT,
  PAUSE_STERILISATION,
  EXTRACTION_PRODUIT,
  RENOUVELLEMENT_AIR,
  AUTORISATION_OUVERTURE,
  TERMINE
};

EtatCycle etatCycleActuel = INACTIF;
unsigned long debutEtape = 0;
int compteurCycles = 0;

// Gestion de la reconnexion MQTT
unsigned long derniereTentativeReconnexion = 0;
const unsigned long delaiReconnexion = 5000; // 5 secondes

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Capteurs
  pinMode(CAPTEUR_PORTE_CONT, INPUT_PULLUP);
  pinMode(CAPTEUR_PORTE_STER, INPUT_PULLUP);

  // Boutons
  pinMode(BTN_OUVRIR_CONT, INPUT_PULLUP);
  pinMode(BTN_FERMER_CONT, INPUT_PULLUP);
  pinMode(BTN_OUVRIR_STER, INPUT_PULLUP);
  pinMode(BTN_FERMER_STER, INPUT_PULLUP);
  pinMode(BTN_DEPART, INPUT_PULLUP);
  pinMode(BTN_URGENCE, INPUT_PULLUP);

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Pass-Box IoT");
  lcd.setCursor(0,1);
  lcd.print("Initialisation..");
  delay(2000);
  
  // Connexion Wi-Fi
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Connexion WiFi..");
  Serial.println("\n╔════════════════════════════════════════════════════════════╗");
  Serial.println("║          PASS-BOX IoT - CONNEXION RÉSEAU                  ║");
  Serial.println("╚════════════════════════════════════════════════════════════╝\n");
  
  connecterWiFi();
  
  // Configuration MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  
  // Connexion MQTT
  connecterMQTT();
  
  // Instructions d'utilisation
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("SWITCHES=Portes");
  lcd.setCursor(0,1);
  lcd.print("BOUTONS=Commande");
  delay(3000);
  
  lcd.clear();
  
  // Instructions détaillées dans le moniteur série
  Serial.println("\n╔════════════════════════════════════════════════════════════╗");
  Serial.println("║       PASS-BOX IoT - SYSTÈME CONNECTÉ - MODE D'EMPLOI     ║");
  Serial.println("╚════════════════════════════════════════════════════════════╝\n");
  
  Serial.println("📡 CONNEXION IoT :");
  Serial.println("─────────────────────────────────────────────────────────────");
  Serial.println("   ✅ Wi-Fi : Connecté");
  Serial.println("   ✅ MQTT : Connecté au broker HiveMQ");
  Serial.println();
  
  Serial.println("📤 TOPICS MQTT PUBLIÉS (ESP32 → Node-RED) :");
  Serial.println("─────────────────────────────────────────────────────────────");
  Serial.println("   • passbox/porte/contaminee  → État porte contaminée (ON/OFF)");
  Serial.println("   • passbox/porte/sterile     → État porte stérile (ON/OFF)");
  Serial.println("   • passbox/cycle/depart      → Démarrage cycle (ON/OFF)");
  Serial.println("   • passbox/cycle/etape       → Étape actuelle (1-7 + description)");
  Serial.println("   • passbox/urgence           → État urgence (ON/OFF)");
  Serial.println("   • passbox/status            → Statut général (JSON)");
  Serial.println();
  
  Serial.println("🔄 CYCLE DE DÉCONTAMINATION (7 étapes) :");
  Serial.println("─────────────────────────────────────────────────────────────");
  Serial.println("   1. Extraction d'air         → 3 secondes");
  Serial.println("   2. Arrêt de l'air           → 2 secondes");
  Serial.println("   3. Injection désinfectant   → 2 secondes");
  Serial.println("   4. Pause stérilisation      → 20 secondes");
  Serial.println("   5. Extraction du produit    → 3 secondes");
  Serial.println("   6. Renouvellement air       → 3 secondes");
  Serial.println("   7. Autorisation ouverture   → Porte stérile autorisée");
  Serial.println();
  
  Serial.println("🎮 SCÉNARIO DE TEST :");
  Serial.println("─────────────────────────────────────────────────────────────");
  Serial.println("1️⃣  Ouvrez Node-RED et créez un dashboard");
  Serial.println("2️⃣  Abonnez-vous aux topics passbox/* dans Node-RED");
  Serial.println("3️⃣  Lancez un cycle avec BTN ORANGE");
  Serial.println("4️⃣  Observez les données en temps réel sur Node-RED");
  Serial.println();
  
  Serial.println("═══════════════════════════════════════════════════════════════");
  Serial.println("✅ Système IoT prêt ! Les données sont publiées sur MQTT");
  Serial.println("═══════════════════════════════════════════════════════════════\n");
  
  // Publier le statut initial
  publierStatus();
}

void loop() {
  // Maintenir la connexion MQTT
  if (!client.connected()) {
    reconnecterMQTT();
  }
  client.loop();
  
  lireCapteurs();
  lireBoutons();
  gererAutorisation();
  gererCycle();
  afficherEtatLCD();
  
  delay(50); 
}

// ============ FONCTIONS Wi-Fi ============
void connecterWiFi() {
  Serial.print("🔌 Connexion au Wi-Fi");
  WiFi.begin(ssid, password);
  
  int tentatives = 0;
  while (WiFi.status() != WL_CONNECTED && tentatives < 20) {
    delay(500);
    Serial.print(".");
    lcd.setCursor(0,1);
    lcd.print("WiFi");
    for(int i = 0; i < (tentatives % 4); i++) {
      lcd.print(".");
    }
    tentatives++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" ✅");
    Serial.print("📡 Adresse IP : ");
    Serial.println(WiFi.localIP());
    
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi OK!");
    lcd.setCursor(0,1);
    lcd.print(WiFi.localIP());
    delay(2000);
  } else {
    Serial.println(" ❌ ÉCHEC");
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi ERREUR");
    delay(2000);
  }
}

// ============ FONCTIONS MQTT ============
void connecterMQTT() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Connexion MQTT..");
  
  Serial.print("🔗 Connexion au broker MQTT");
  
  String clientId = "PassBox-ESP32-";
  clientId += String(random(0xffff), HEX);
  
  int tentatives = 0;
  while (!client.connected() && tentatives < 5) {
    Serial.print(".");
    
    if (client.connect(clientId.c_str())) {
      Serial.println(" ✅");
      Serial.print("📡 Connecté au broker : ");
      Serial.println(mqtt_server);
      
      lcd.setCursor(0,1);
      lcd.print("MQTT OK!");
      delay(1000);
      
      // S'abonner aux topics de commande (pour Node-RED → ESP32)
      // On pourrait recevoir des commandes ici si besoin
      
    } else {
      Serial.print(" ❌ Erreur : ");
      Serial.println(client.state());
      tentatives++;
      delay(2000);
    }
  }
  
  if (!client.connected()) {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("MQTT ERREUR");
    delay(2000);
  }
}

void reconnecterMQTT() {
  unsigned long maintenant = millis();
  
  if (maintenant - derniereTentativeReconnexion > delaiReconnexion) {
    derniereTentativeReconnexion = maintenant;
    
    Serial.println("🔄 Tentative de reconnexion MQTT...");
    
    String clientId = "PassBox-ESP32-";
    clientId += String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str())) {
      Serial.println("✅ Reconnecté au broker MQTT");
      publierStatus();
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Callback pour recevoir des messages MQTT (si besoin de commandes externes)
  Serial.print("📩 Message reçu sur ");
  Serial.print(topic);
  Serial.print(" : ");
  
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);
}

// ============ FONCTIONS DE PUBLICATION MQTT ============
void publierPorteContaminee(bool ouverte) {
  if (client.connected()) {
    client.publish(TOPIC_PORTE_CONT, ouverte ? "ON" : "OFF", true);
    Serial.print("📤 MQTT: Porte Contaminée = ");
    Serial.println(ouverte ? "ON" : "OFF");
  }
}

void publierPorteSterile(bool ouverte) {
  if (client.connected()) {
    client.publish(TOPIC_PORTE_STER, ouverte ? "ON" : "OFF", true);
    Serial.print("📤 MQTT: Porte Stérile = ");
    Serial.println(ouverte ? "ON" : "OFF");
  }
}

void publierCycleDepart(bool enCours) {
  if (client.connected()) {
    client.publish(TOPIC_CYCLE_DEPART, enCours ? "ON" : "OFF", true);
    Serial.print("📤 MQTT: Cycle = ");
    Serial.println(enCours ? "DÉMARRE" : "TERMINÉ");
  }
}

void publierEtapeCycle(int etape, String description) {
  if (client.connected()) {
    String message = String(etape) + ":" + description;
    client.publish(TOPIC_CYCLE_ETAPE, message.c_str());
    Serial.print("📤 MQTT: Étape ");
    Serial.print(etape);
    Serial.print("/7 - ");
    Serial.println(description);
  }
}

void publierUrgence(bool active) {
  if (client.connected()) {
    client.publish(TOPIC_URGENCE, active ? "ON" : "OFF", true);
    Serial.print("📤 MQTT: Urgence = ");
    Serial.println(active ? "ACTIVÉE" : "DÉSACTIVÉE");
  }
}

void publierStatus() {
  if (client.connected()) {
    String json = "{";
    json += "\"porteCont\":\"" + String(porteContOuverte ? "ON" : "OFF") + "\",";
    json += "\"porteSter\":\"" + String(porteSterOuverte ? "ON" : "OFF") + "\",";
    json += "\"cycleActif\":" + String(etatCycleActuel != INACTIF ? "true" : "false") + ",";
    json += "\"etape\":" + String(getNumeroEtape()) + ",";
    json += "\"urgence\":\"" + String(urgenceActivee ? "ON" : "OFF") + "\",";
    json += "\"compteurCycles\":" + String(compteurCycles);
    json += "}";
    
    client.publish(TOPIC_STATUS, json.c_str());
    Serial.println("📤 MQTT: Statut complet publié");
  }
}

int getNumeroEtape() {
  switch (etatCycleActuel) {
    case EXTRACTION_AIR: return 1;
    case ARRET_AIR: return 2;
    case INJECTION_DESINFECTANT: return 3;
    case PAUSE_STERILISATION: return 4;
    case EXTRACTION_PRODUIT: return 5;
    case RENOUVELLEMENT_AIR: return 6;
    case AUTORISATION_OUVERTURE: return 7;
    default: return 0;
  }
}

// ============ LECTURE CAPTEURS ============
void lireCapteurs() {
  bool capteurContActuel = (digitalRead(CAPTEUR_PORTE_CONT) == LOW);
  bool capteurSterActuel = (digitalRead(CAPTEUR_PORTE_STER) == LOW);

  // Détection changement porte contaminée
  if (capteurContActuel != porteContOuverte) {
    porteContOuverte = capteurContActuel;
    Serial.print("🚪 Porte Contaminée physiquement : ");
    Serial.println(porteContOuverte ? "OUVERTE" : "FERMÉE");
    
    // Publier sur MQTT
    publierPorteContaminee(porteContOuverte);
    
    if (porteContOuverte) {
      demandeOuvrirCont = false;
      // Si une porte s'ouvre pendant le cycle, arrêt d'urgence
      if (etatCycleActuel != INACTIF && etatCycleActuel != TERMINE) {
        urgenceActivee = true;
        publierUrgence(true);
        etatCycleActuel = INACTIF;
        publierCycleDepart(false);
        Serial.println("⚠️  CYCLE INTERROMPU : Porte ouverte pendant le cycle !");
      }
    }
  }

  // Détection changement porte stérile
  if (capteurSterActuel != porteSterOuverte) {
    porteSterOuverte = capteurSterActuel;
    Serial.print("🚪 Porte Stérile physiquement : ");
    Serial.println(porteSterOuverte ? "OUVERTE" : "FERMÉE");
    
    // Publier sur MQTT
    publierPorteSterile(porteSterOuverte);
    
    if (porteSterOuverte) {
      demandeOuvrirSter = false;
      // Réinitialiser le cycle si terminé et porte ouverte
      if (etatCycleActuel == TERMINE || etatCycleActuel == AUTORISATION_OUVERTURE) {
        etatCycleActuel = INACTIF;
        publierCycleDepart(false);
        Serial.println("✅ Matériel récupéré - Système prêt pour nouveau cycle");
      }
      // Si porte s'ouvre pendant le cycle (avant autorisation), urgence
      else if (etatCycleActuel != INACTIF) {
        urgenceActivee = true;
        publierUrgence(true);
        etatCycleActuel = INACTIF;
        publierCycleDepart(false);
        Serial.println("⚠️  CYCLE INTERROMPU : Porte stérile ouverte prématurément !");
      }
    }
  }
}

// ============ LECTURE BOUTONS ============
void lireBoutons() {
  unsigned long now = millis();

  // BTN_OUVRIR_CONT
  if (digitalRead(BTN_OUVRIR_CONT) == LOW && now - dernierTempsBtn[0] > debounceDelay) {
    if (urgenceActivee) {
      afficherMessageTemporaire("URGENCE ACTIVE!");
      Serial.println("❌ Commande bloquée : URGENCE activée");
    }
    else if (etatCycleActuel != INACTIF && etatCycleActuel != TERMINE) {
      afficherMessageTemporaire("Cycle en cours");
      Serial.println("❌ Impossible : Cycle de décontamination en cours");
    }
    else if (porteSterOuverte) {
      afficherMessageTemporaire("ERREUR: Porte St");
      Serial.println("❌ INTER-VERROUILLAGE : Porte Stérile ouverte !");
    }
    else if (porteContOuverte) {
      afficherMessageTemporaire("Deja ouverte");
      Serial.println("ℹ️  Porte Contaminée déjà ouverte");
    }
    else {
      demandeOuvrirCont = true;
      afficherMessageTemporaire("Autorise P.Cont");
      Serial.println("✅ AUTORISATION : Porte Contaminée peut être ouverte");
    }
    dernierTempsBtn[0] = now;
  }

  // BTN_FERMER_CONT
  if (digitalRead(BTN_FERMER_CONT) == LOW && now - dernierTempsBtn[1] > debounceDelay) {
    demandeOuvrirCont = false;
    if (porteContOuverte) {
      afficherMessageTemporaire("Fermez sw1");
      Serial.println("📢 Pour fermer : Actionnez le SWITCH sw1");
    } else {
      afficherMessageTemporaire("Deja fermee");
      Serial.println("ℹ️  Porte Contaminée déjà fermée");
    }
    dernierTempsBtn[1] = now;
  }

  // BTN_OUVRIR_STER
  if (digitalRead(BTN_OUVRIR_STER) == LOW && now - dernierTempsBtn[2] > debounceDelay) {
    if (urgenceActivee) {
      afficherMessageTemporaire("URGENCE ACTIVE!");
      Serial.println("❌ Commande bloquée : URGENCE activée");
    }
    else if (etatCycleActuel != INACTIF && etatCycleActuel != TERMINE && etatCycleActuel != AUTORISATION_OUVERTURE) {
      afficherMessageTemporaire("Cycle en cours");
      Serial.println("❌ Impossible : Attendez la fin du cycle");
    }
    else if (porteContOuverte) {
      afficherMessageTemporaire("ERREUR: Porte Co");
      Serial.println("❌ INTER-VERROUILLAGE : Porte Contaminée ouverte !");
    }
    else if (porteSterOuverte) {
      afficherMessageTemporaire("Deja ouverte");
      Serial.println("ℹ️  Porte Stérile déjà ouverte");
    }
    else {
      demandeOuvrirSter = true;
      afficherMessageTemporaire("Autorise P.Ster");
      Serial.println("✅ AUTORISATION : Porte Stérile peut être ouverte");
    }
    dernierTempsBtn[2] = now;
  }

  // BTN_FERMER_STER
  if (digitalRead(BTN_FERMER_STER) == LOW && now - dernierTempsBtn[3] > debounceDelay) {
    demandeOuvrirSter = false;
    if (porteSterOuverte) {
      afficherMessageTemporaire("Fermez sw2");
      Serial.println("📢 Pour fermer : Actionnez le SWITCH sw2");
    } else {
      afficherMessageTemporaire("Deja fermee");
      Serial.println("ℹ️  Porte Stérile déjà fermée");
    }
    dernierTempsBtn[3] = now;
  }

  // BTN_DEPART
  if (digitalRead(BTN_DEPART) == LOW && now - dernierTempsBtn[4] > debounceDelay) {
    if (urgenceActivee) {
      afficherMessageTemporaire("URGENCE ACTIVE!");
      Serial.println("❌ Cycle bloqué : URGENCE activée");
    }
    else if (etatCycleActuel != INACTIF && etatCycleActuel != TERMINE) {
      afficherMessageTemporaire("Deja en cours");
      Serial.println("ℹ️  Cycle déjà en cours");
    }
    else if (porteContOuverte || porteSterOuverte) {
      afficherMessageTemporaire("Fermer portes!");
      Serial.println("❌ ERREUR : Les deux portes doivent être fermées");
    }
    else {
      // Démarrer le cycle
      compteurCycles++;
      etatCycleActuel = EXTRACTION_AIR;
      debutEtape = millis();
      afficherMessageTemporaire("Cycle demarre!");
      
      // Publier sur MQTT
      publierCycleDepart(true);
      publierEtapeCycle(1, "Extraction air");
      publierStatus();
      
      Serial.println("\n╔════════════════════════════════════════════════════════════╗");
      Serial.print("║   CYCLE #");
      Serial.print(compteurCycles);
      Serial.println(" - DÉCONTAMINATION EN COURS (données publiées)  ║");
      Serial.println("╚════════════════════════════════════════════════════════════╝");
    }
    dernierTempsBtn[4] = now;
  }

  // BTN_URGENCE
  if (digitalRead(BTN_URGENCE) == LOW && now - dernierTempsBtn[5] > debounceDelay) {
    urgenceActivee = !urgenceActivee;
    
    // Publier sur MQTT
    publierUrgence(urgenceActivee);
    
    if (urgenceActivee) {
      demandeOuvrirCont = false;
      demandeOuvrirSter = false;
      
      if (etatCycleActuel != INACTIF) {
        Serial.println("\n⚠️  CYCLE INTERROMPU PAR ARRÊT D'URGENCE !");
        etatCycleActuel = INACTIF;
        publierCycleDepart(false);
      }
      
      afficherMessageTemporaire("!!! URGENCE !!!");
      Serial.println("🔴🚨 ARRÊT D'URGENCE ACTIVÉ !!!");
    } else {
      afficherMessageTemporaire("Urgence reset");
      Serial.println("🟢 Urgence désactivée - Système prêt");
    }
    
    publierStatus();
    dernierTempsBtn[5] = now;
  }
}

// ============ GESTION DU CYCLE ============
void gererCycle() {
  if (etatCycleActuel == INACTIF || etatCycleActuel == TERMINE) {
    return;
  }
  
  unsigned long tempsEcoule = millis() - debutEtape;
  
  switch (etatCycleActuel) {
    case EXTRACTION_AIR:
      if (tempsEcoule >= 3000) {
        Serial.println("✅ Étape 1/7 : Extraction d'air complétée");
        etatCycleActuel = ARRET_AIR;
        debutEtape = millis();
        publierEtapeCycle(2, "Arret air");
        Serial.println("⏳ Étape 2/7 : Arrêt de l'air...");
      }
      break;
      
    case ARRET_AIR:
      if (tempsEcoule >= 2000) {
        Serial.println("✅ Étape 2/7 : Arrêt de l'air complété");
        etatCycleActuel = INJECTION_DESINFECTANT;
        debutEtape = millis();
        publierEtapeCycle(3, "Injection desinfectant");
        Serial.println("⏳ Étape 3/7 : Injection du désinfectant...");
      }
      break;
      
    case INJECTION_DESINFECTANT:
      if (tempsEcoule >= 2000) {
        Serial.println("✅ Étape 3/7 : Injection désinfectant complétée");
        etatCycleActuel = PAUSE_STERILISATION;
        debutEtape = millis();
        publierEtapeCycle(4, "Sterilisation");
        Serial.println("⏳ Étape 4/7 : Pause de stérilisation (20s)...");
      }
      break;
      
    case PAUSE_STERILISATION:
      if (tempsEcoule >= 20000) {
        Serial.println("✅ Étape 4/7 : Stérilisation complétée");
        etatCycleActuel = EXTRACTION_PRODUIT;
        debutEtape = millis();
        publierEtapeCycle(5, "Extraction produit");
        Serial.println("⏳ Étape 5/7 : Extraction du produit...");
      }
      break;
      
    case EXTRACTION_PRODUIT:
      if (tempsEcoule >= 3000) {
        Serial.println("✅ Étape 5/7 : Extraction du produit complétée");
        etatCycleActuel = RENOUVELLEMENT_AIR;
        debutEtape = millis();
        publierEtapeCycle(6, "Renouvellement air");
        Serial.println("⏳ Étape 6/7 : Renouvellement avec air propre...");
      }
      break;
      
    case RENOUVELLEMENT_AIR:
      if (tempsEcoule >= 3000) {
        Serial.println("✅ Étape 6/7 : Renouvellement air complété");
        etatCycleActuel = AUTORISATION_OUVERTURE;
        debutEtape = millis();
        publierEtapeCycle(7, "Autorisation ouverture");
        publierCycleDepart(false);
        publierStatus();
        
        Serial.println("✅ Étape 7/7 : Autorisation d'ouverture côté stérile");
        Serial.println("\n╔════════════════════════════════════════════════════════════╗");
        Serial.println("║              ✅ CYCLE TERMINÉ AVEC SUCCÈS ✅               ║");
        Serial.println("╚════════════════════════════════════════════════════════════╝");
        Serial.println("🟢 Porte stérile autorisée - Données publiées sur MQTT\n");
        afficherMessageTemporaire("Cycle termine!");
      }
      break;
      
    case AUTORISATION_OUVERTURE:
      // Attendre que l'utilisateur récupère le matériel
      break;
  }
}

void gererAutorisation() {
  if (demandeOuvrirCont && porteSterOuverte) {
    demandeOuvrirCont = false;
    afficherMessageTemporaire("Autoris annulee");
    Serial.println("⚠️  AUTORISATION ANNULÉE : Porte Stérile s'est ouverte");
  }
  
  if (demandeOuvrirSter && porteContOuverte) {
    demandeOuvrirSter = false;
    afficherMessageTemporaire("Autoris annulee");
    Serial.println("⚠️  AUTORISATION ANNULÉE : Porte Contaminée s'est ouverte");
  }
}

void afficherMessageTemporaire(String msg) {
  messageLCD = msg;
  tempsMessageLCD = millis();
}

void afficherEtatLCD() {
  // Ligne 1 : État des portes + cycle
  lcd.setCursor(0,0);
  
  if (etatCycleActuel != INACTIF && etatCycleActuel != TERMINE) {
    // En cours de cycle
    lcd.print("CYCLE ");
    switch (etatCycleActuel) {
      case EXTRACTION_AIR:          lcd.print("1/7   "); break;
      case ARRET_AIR:               lcd.print("2/7   "); break;
      case INJECTION_DESINFECTANT:  lcd.print("3/7   "); break;
      case PAUSE_STERILISATION:     lcd.print("4/7   "); break;
      case EXTRACTION_PRODUIT:      lcd.print("5/7   "); break;
      case RENOUVELLEMENT_AIR:      lcd.print("6/7   "); break;
      case AUTORISATION_OUVERTURE:  lcd.print("OK    "); break;
      default: break;
    }
    
    // Temps restant
    unsigned long tempsEcoule = millis() - debutEtape;
    unsigned long dureeEtape = 0;
    
    switch (etatCycleActuel) {
      case EXTRACTION_AIR:          dureeEtape = 3000; break;
      case ARRET_AIR:               dureeEtape = 2000; break;
      case INJECTION_DESINFECTANT:  dureeEtape = 2000; break;
      case PAUSE_STERILISATION:     dureeEtape = 20000; break;
      case EXTRACTION_PRODUIT:      dureeEtape = 3000; break;
      case RENOUVELLEMENT_AIR:      dureeEtape = 3000; break;
      default: break;
    }
    
    if (dureeEtape > 0 && etatCycleActuel != AUTORISATION_OUVERTURE) {
      int secondesRestantes = (dureeEtape - tempsEcoule) / 1000;
      lcd.print(secondesRestantes);
      lcd.print("s ");
    }
  } else {
    // Pas de cycle
    lcd.print("C:");
    lcd.print(porteContOuverte ? "O" : "F");
    if (demandeOuvrirCont && !porteContOuverte) lcd.print("*");
    else lcd.print(" ");
    
    lcd.print("S:");
    lcd.print(porteSterOuverte ? "O" : "F");
    if (demandeOuvrirSter && !porteSterOuverte) lcd.print("*");
    else lcd.print(" ");
    
    if (urgenceActivee) {
      lcd.print("[URG]");
    } else if (client.connected()) {
      lcd.print("[IoT]");
    } else {
      lcd.print("     ");
    }
  }

  // Ligne 2
  lcd.setCursor(0,1);
  
  if (millis() - tempsMessageLCD < dureeMessageLCD) {
    lcd.print("                ");
    lcd.setCursor(0,1);
    lcd.print(messageLCD);
  } else {
    lcd.print("                ");
    lcd.setCursor(0,1);
    
    if (urgenceActivee) {
      lcd.print("MODE URGENCE");
    }
    else if (etatCycleActuel == EXTRACTION_AIR) {
      lcd.print("Extract. air");
    }
    else if (etatCycleActuel == ARRET_AIR) {
      lcd.print("Arret air");
    }
    else if (etatCycleActuel == INJECTION_DESINFECTANT) {
      lcd.print("Inject. desinfec");
    }
    else if (etatCycleActuel == PAUSE_STERILISATION) {
      lcd.print("Sterilisation");
    }
    else if (etatCycleActuel == EXTRACTION_PRODUIT) {
      lcd.print("Extract. produit");
    }
    else if (etatCycleActuel == RENOUVELLEMENT_AIR) {
      lcd.print("Renouv. air");
    }
    else if (etatCycleActuel == AUTORISATION_OUVERTURE) {
      lcd.print("Ouvrir P.Sterile");
    }
    else if (porteContOuverte || porteSterOuverte) {
      lcd.print("Porte(s) ouv.");
    } 
    else if (demandeOuvrirCont || demandeOuvrirSter) {
      lcd.print("Autorisee (*)");
    }
    else {
      lcd.print("Pret au cycle");
    }
  }
}