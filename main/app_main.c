#include <stdio.h>  
#include "esp_log.h"
#include "driver/gpio.h"
#include "dht.h"  // Biblioteca do DHT
#include "esp_http_client.h"
#include "freertos/task.h"
#include "time.h"
#include "esp_timer.h"
#include <WiFi.h>  


// Definição dos pinos
#define DHT22_PIN GPIO_NUM_4  // Pino onde o DHT22 está conectado
#define FAN_PIN GPIO_NUM_14    // Pino para o ventilador
#define PUMP_GPIO GPIO_NUM_5   // Pino para a bomba
#define BUZZER_PIN GPIO_NUM_17 // Pino para o buzzer

// Definição dos pinos do sensor ultrassônico
#define TRIG_PIN GPIO_NUM_15   // Pino para o TRIG do sensor ultrassônico
#define ECHO_PIN GPIO_NUM_16   // Pino para o ECHO do sensor ultrassônico

// Configurações do reservatório e do sistema
#define RESERVOIR_HEIGHT 20.0   // Altura máxima do reservatório em cm
#define MAX_VOLUME 10.0         // Volume máximo do reservatório em litros
#define LOW_WATER_THRESHOLD 2.0 // Limite crítico de volume de água em litros

// Configurações do Telegram
#define BOT_TOKEN "8095372437:AAEo-fqpa7lU-PWlvls8MVvbSuQ2Z_sQfAY"
#define CHAT_ID "7805552487"

// Configurações do Modo de Emergência
#define MAINTENANCE_DAY_INTERVAL 7    // Intervalo de dias para manutenção
#define MAINTENANCE_DURATION_SECONDS 3600 // Duração da manutenção em segundos (1 hora)

// Variáveis globais de controle
static const char *TAG = "GreenLeaf";
bool critical_alert_sent = false;
bool maintenance_mode = false;
time_t maintenance_start_time = 0;

// Definição das credenciais Wi-Fi
#define SSID "SuaRedeWiFi"  // Substitua pelo nome da sua rede Wi-Fi
#define PASSWORD "SuaSenhaWiFi"  // Substitua pela senha da sua rede Wi-Fi

// Função para conectar ao Wi-Fi
void connect_wifi() {
    WiFi.begin(SSID, PASSWORD);  // Conecta à rede Wi-Fi

    // Aguarda até conseguir conectar à rede Wi-Fi
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);  // Aguarda 1 segundo
        ESP_LOGI(TAG, "Conectando ao Wi-Fi...");
    }

    // Quando conectado
    ESP_LOGI(TAG, "Conectado ao Wi-Fi!");
    ESP_LOGI(TAG, "Endereço IP: %s", WiFi.localIP().toString().c_str());
}

// Função para enviar mensagem ao Telegram
void send_telegram_message(const char *message) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s",
             BOT_TOKEN, CHAT_ID, message);

    // Realizar requisição HTTP para o Telegram (GET simples)
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Mensagem enviada ao Telegram com sucesso!");
    } else {
        ESP_LOGE(TAG, "❌ Erro ao enviar mensagem ao Telegram: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

// Função para ler o DHT22 e retornar tanto a temperatura quanto a umidade
void read_dht22(float *temperature, float *humidity) {
    int16_t temp = 0;
    int16_t hum = 0;
    esp_err_t ret = dht_read_data(DHT_TYPE_DHT22, DHT22_PIN, &hum, &temp);
    
    if (ret == ESP_OK) {
        *temperature = temp / 10.0;  // Temperatura em °C
        *humidity = hum / 10.0;      // Umidade em %
        ESP_LOGI(TAG, "🌡️ Temperatura: %.1f°C, 💧 Umidade: %.1f%%", *temperature, *humidity);
    } else {
        *temperature = -999.0;  // Valor de erro para temperatura
        *humidity = -999.0;     // Valor de erro para umidade
        ESP_LOGE(TAG, "❌ Falha ao ler o DHT22");
    }
}

// Função para controlar o ventilador com histerese
void control_fan(float temperature) {
    static float last_temperature = -1.0;

    if (temperature > 30.0 && temperature != last_temperature) {
        gpio_set_level(FAN_PIN, 1);  // Liga o ventilador
        ESP_LOGI(TAG, "💨 Ventilador ligado.");
    } else if (temperature < 28.0 && temperature != last_temperature) {
        gpio_set_level(FAN_PIN, 0);  // Desliga o ventilador
        ESP_LOGI(TAG, "💨 Ventilador desligado.");
    }

    last_temperature = temperature;
}

// Função para calcular o volume de água no reservatório
float calculate_water_volume(float distance) {
    float water_height = RESERVOIR_HEIGHT - distance;
    if (water_height < 0) water_height = 0;
    float volume = (water_height / RESERVOIR_HEIGHT) * MAX_VOLUME;
    return volume;
}

// Função para controlar a bomba de água
void control_pump(float volume) {
    if (volume < LOW_WATER_THRESHOLD) {
        if (!critical_alert_sent) {
            gpio_set_level(PUMP_GPIO, 0);  // Desliga a bomba
            send_telegram_message("⚠️ Nível de água crítico! A bomba foi desligada.");
            sound_buzzer(1000);  // Toca o buzzer por 1 segundo
            critical_alert_sent = true;
            ESP_LOGI(TAG, "❌ Bomba desligada devido ao nível crítico de água.");
        }
    } else {
        gpio_set_level(PUMP_GPIO, 1);  // Liga a bomba
        critical_alert_sent = false;
        ESP_LOGI(TAG, "💦 Bomba ligada.");
    }
}

// Função para controlar o sensor ultrassônico
float read_ultrasonic_distance() {
    gpio_set_level(TRIG_PIN, 0);
    ets_delay_us(2);  // Pequeno atraso para estabilizar o pulso
    gpio_set_level(TRIG_PIN, 1);
    ets_delay_us(10);  // Pulso de 10 µs
    gpio_set_level(TRIG_PIN, 0);

    int64_t start_time = esp_timer_get_time();
    while (gpio_get_level(ECHO_PIN) == 0 && (esp_timer_get_time() - start_time) < 20000);

    if (gpio_get_level(ECHO_PIN) == 0) {
        ESP_LOGE(TAG, "❌ Falha na leitura do ECHO (timeout)");
        return -1;  // Erro
    }

    start_time = esp_timer_get_time();
    while (gpio_get_level(ECHO_PIN) == 1 && (esp_timer_get_time() - start_time) < 20000);

    if (gpio_get_level(ECHO_PIN) == 1) {
        ESP_LOGE(TAG, "❌ Falha no fim do pulso do ECHO (timeout)");
        return -1;  // Erro
    }

    int64_t end_time = esp_timer_get_time();
    float duration = (float)(end_time - start_time);
    float distance_cm = (duration / 2.0) * 0.0343;  // Calcula a distância em cm
    return distance_cm;
}

// Função para tocar o buzzer
void sound_buzzer(int duration_ms) {
    gpio_set_level(BUZZER_PIN, 1);  // Liga o buzzer
    vTaskDelay(duration_ms / portTICK_PERIOD_MS); // Aguarda o tempo definido
    gpio_set_level(BUZZER_PIN, 0);  // Desliga o buzzer
}

// Função de manutenção programada (desliga a bomba por 1 hora a cada 7 dias)
void handle_maintenance_mode(time_t now, int days_elapsed) {
    if (!maintenance_mode && days_elapsed % MAINTENANCE_DAY_INTERVAL == 0) {
        // Inicia a manutenção
        maintenance_mode = true;
        maintenance_start_time = now; // Salva o tempo de início
        gpio_set_level(PUMP_GPIO, 0); // Desliga a bomba
        send_telegram_message("🔧 Modo de manutenção ativado. A bomba foi desligada por 1 hora.");
    }

    if (maintenance_mode) {
        // Verifica se o tempo de manutenção terminou
        if ((now - maintenance_start_time) >= MAINTENANCE_DURATION_SECONDS) {
            maintenance_mode = false;
            gpio_set_level(PUMP_GPIO, 1); // Religa a bomba
            send_telegram_message("🔧 Manutenção concluída. A bomba foi religada.");
        }
    }
}

// Função principal
void app_main(void) {
    // Conectar ao Wi-Fi
    connect_wifi();

    // Inicialização dos pinos
    gpio_set_direction(DHT22_PIN, GPIO_MODE_INPUT);  // Pino DHT22 como entrada
    gpio_reset_pin(DHT22_PIN);
    gpio_set_direction(FAN_PIN, GPIO_MODE_OUTPUT);   // Pino do ventilador como saída
    gpio_reset_pin(FAN_PIN);
    gpio_set_direction(PUMP_GPIO, GPIO_MODE_OUTPUT); // Pino da bomba como saída
    gpio_reset_pin(PUMP_GPIO);
    gpio_set_direction(TRIG_PIN, GPIO_MODE_OUTPUT);  // Pino TRIG do sensor ultrassônico
    gpio_reset_pin(TRIG_PIN);
    gpio_set_direction(ECHO_PIN, GPIO_MODE_INPUT);   // Pino ECHO do sensor ultrassônico
    gpio_reset_pin(ECHO_PIN);
    gpio_set_direction(BUZZER_PIN, GPIO_MODE_OUTPUT);  // Pino do buzzer como saída
    gpio_reset_pin(BUZZER_PIN);

    // Inicializa o tempo de início
    time_t start_time;
    time(&start_time); // Tempo inicial para contagem de dias

    while (1) {
        // Leitura do sensor DHT22 (temperatura e umidade)
        float temperature, humidity;
        read_dht22(&temperature, &humidity);
        
        // Controle do ventilador
        control_fan(temperature);
        
        // Leitura do sensor ultrassônico (distância da água)
        float distance = read_ultrasonic_distance();
        if (distance != -1) {
            float volume = calculate_water_volume(distance); // Cálculo do volume de água
            control_pump(volume);
        }

        // Controle do modo de manutenção programado
        time_t now;
        time(&now);
        int days_elapsed = (now - start_time) / (24 * 60 * 60); // Dias passados
        handle_maintenance_mode(now, days_elapsed);

        vTaskDelay(5000 / portTICK_PERIOD_MS); // Delay de 5 segundos entre leituras
    }
}
