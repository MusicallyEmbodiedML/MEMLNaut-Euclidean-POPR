#include "src/memllib/audio/AudioDriver.hpp"
#include "src/memllib/hardware/memlnaut/MEMLNaut.hpp"
#include <memory>
#include "src/memllib/interface/MIDIInOut.hpp"
#include "src/memllib/PicoDefs.hpp"
#include "src/memllib/interface/UARTInput.hpp"
#include "pico/util/queue.h"

// Example apps and interfaces
#include "src/memllib/examples/IMLInterface.hpp"
//#include "src/memllib/examples/InterfaceRL.hpp"

#include "src/memllib/hardware/memlnaut/display.hpp"


#include "src/memllib/examples/EuclideanAudioApp.hpp"
#include "src/memllib/examples/EuclideanMIDI.hpp"
#include "src/memllib/interface/USeqI2C.hpp"


/******************************* */

const char FIRMWARE_NAME[] = "-- Euclidean sequencer POPR --";

// Global objects
using CURRENT_AUDIO_APP = EuclideanAudioApp;
using CURRENT_INTERFACE = IMLInterface;
std::shared_ptr<CURRENT_INTERFACE> interface;
std::shared_ptr<CURRENT_AUDIO_APP> audio_app;
std::shared_ptr<MIDIInOut> midi_interf;
std::shared_ptr<UARTInput> uart_input;
std::shared_ptr<display> disp;
std::unique_ptr<EuclideanMIDI> euclidean_midi;
std::unique_ptr<USeqI2C> useq_i2c;

// Inter-core communication
volatile bool core_0_ready = false;
volatile bool core_1_ready = false;
volatile bool serial_ready = false;
volatile bool interface_ready = false;

// We're only bound to the joystick inputs (x, y, rotate)
const size_t kN_InputParams = 3;
const std::vector<size_t> kUARTListenInputs {};

// LED control
volatile bool led_state = false;
volatile bool enable_heartbeat_led = false; // Disabled by default

// Queue for euclidean data transfer between cores
struct euclidean_data_t {
    float generators[EuclideanAudioApp::kN_Operators];
};
queue_t euclidean_queue;


void setup()
{
    Serial.begin(115200);
    //while (!Serial) {}
    Serial.println("Serial initialised.");
    WRITE_VOLATILE(serial_ready, true);

    // Initialize inter-core queue
    queue_init(&euclidean_queue, sizeof(euclidean_data_t), 4);

    // Setup board
    MEMLNaut::Initialize();
    pinMode(33, OUTPUT);
    digitalWrite(33, LOW); // Initialize LED as off

    // Move MIDI setup after Serial is confirmed ready
    Serial.println("Initializing MIDI...");
    midi_interf = std::make_shared<MIDIInOut>();
    midi_interf->Setup(CURRENT_AUDIO_APP::kN_Params);
    //midi_interf->SetMIDISendChannel(1);
    Serial.println("MIDI setup complete.");
    midi_interf->SetAdvancedParamMappings({
        // 1
        {16, 1, 0, 127}, // Tune
        {18, 1, 0, 127}, // Bit crush
        {74, 1, 0, 60}, // Filt freq
        {75, 1, 0, 127}, // Filt reso
        // 2
        {16, 2, 0, 127},
        {18, 2, 0, 127},
        {74, 2, 70, 127},
        {75, 2, 0, 127},
        // 3
        {16, 3, 0, 127},
        {23, 3, 0, 127}, // Sample level
        {74, 3, 70, 127},
        {75, 3, 0, 127},
        // 4
        {16, 4, 0, 127},
        {23, 4, 0, 127},
        {74, 4, 70, 127},
        {75, 4, 0, 127},
        // 5
        {16, 5, 0, 127},
        {23, 5, 0, 127}, // Sample level
        {74, 5, 70, 127},
        {75, 5, 0, 127},
        // 6
        {16, 6, 0, 127},
        {23, 6, 0, 127},
        {74, 6, 70, 127},
        {75, 6, 0, 127}
    });

    delay(100); // Allow Serial2 to stabilize

    // Setup UART input
    uart_input = std::make_shared<UARTInput>(kUARTListenInputs);
    const size_t total_input_params = kN_InputParams + kUARTListenInputs.size();

    // Setup display
    disp = std::make_shared<display>();
    disp->setup();
    disp->post(FIRMWARE_NAME);

    // Set up euclidean outputs: I2C
    useq_i2c = std::make_unique<USeqI2C>();
    if (!useq_i2c->begin()) {
        Serial.println("Failed to initialize USeq I2C interface.");
    } else {
        Serial.println("USeq I2C interface initialized successfully.");
    }
    // ...and MIDI
    euclidean_midi = std::make_unique<EuclideanMIDI>();
    // Note configurations: range from C4 to C4 + kMaxOperators - 1, with channels 1 to kMaxOperators
    std::vector<MIDINoteConfig> note_configs(EuclideanMIDI::kMaxOperators);
    for (size_t i = 0; i < EuclideanMIDI::kMaxOperators; ++i) {
        note_configs[i] = MIDINoteConfig(60 + i, i + 1); // C4 + i on channel i + 1
    }
    // Sequence of fifths starting from C3 (48)
    note_configs[0] = {48, 1};  // C3 - Root
    note_configs[1] = {60, 2};  // G3 - Fifth
    note_configs[2] = {55, 3};  // D4 - Ninth (second fifth)
    note_configs[3] = {60, 4};  // A4 - Thirteenth (third fifth)
    note_configs[4] = {60, 5};  // E5 - Seventeenth (fourth fifth)
    note_configs[5] = {60, 6};  // B5 - Twenty-first (fifth fifth)
    note_configs[6] = {60, 7};  // F#6 - Twenty-fifth (sixth fifth)
    note_configs[7] = {60, 8};  // C#7 - Twenty-ninth (seventh fifth)
    euclidean_midi->Setup(midi_interf, note_configs);
    Serial.println("Euclidean MIDI interface initialized.");

    // Setup interface with memory barrier protection
    {
        auto temp_interface = std::make_shared<CURRENT_INTERFACE>();
        MEMORY_BARRIER();
        temp_interface->setup(total_input_params, CURRENT_AUDIO_APP::kN_Params, disp);
        MEMORY_BARRIER();
        temp_interface->SetMIDIInterface(midi_interf);
        MEMORY_BARRIER();
        interface = temp_interface;
        MEMORY_BARRIER();
    }
    WRITE_VOLATILE(interface_ready, true);

    // Bind interface after ensuring it's fully initialized
    interface->bindInterface();
    Serial.println("Bound interface to MEMLNaut.");
    interface->bindUARTInput(uart_input, kUARTListenInputs);
    Serial.println("Bound interface to UART input.");
    interface->bindMIDI(midi_interf);
    Serial.println("Bound interface to MIDI input.");

    WRITE_VOLATILE(core_0_ready, true);
    while (!READ_VOLATILE(core_1_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    Serial.println("Finished initialising core 0.");
}

void loop()
{
    static uint32_t last_1ms = 0;
    static uint32_t last_10ms = 0;
    uint32_t current_time = micros();

    // Tasks to run as fast as possible
    {
        // Poll the UART input
        uart_input->Poll();
        // Poll the MIDI interface
        midi_interf->Poll();

        // Check for euclidean data from core 1
        euclidean_data_t euclidean_data;
        if (queue_try_remove(&euclidean_queue, &euclidean_data)) {
            // Select generator 1 (index 0) to drive the LED
            bool generator_1_active = (euclidean_data.generators[0] > 0.0f);
            WRITE_VOLATILE(led_state, generator_1_active);
            // Send data to I2C interface
            if (useq_i2c && useq_i2c->isInitialized()) {
                useq_i2c->sendValues(euclidean_data.generators, EuclideanAudioApp::kN_Operators);
            }
            // Send data to MIDI interface
            if (euclidean_midi && euclidean_midi->IsConfigured()) {
                euclidean_midi->ProcessCV(
                    std::vector<float>(euclidean_data.generators,
                    euclidean_data.generators + EuclideanAudioApp::kN_Operators)
                );
            }
        }

        // Update LED based on euclidean generator state
        digitalWrite(33, READ_VOLATILE(led_state) ? HIGH : LOW);
    }

    // Tasks to run every 1ms
    if (current_time - last_1ms >= 1000) {
        last_1ms = current_time;

        // None for now
    }

    // Tasks to run every 10ms
    if (current_time - last_10ms >= 10000) {
        last_10ms = current_time;

        // Poll HAL
        MEMORY_BARRIER();
        MEMLNaut::Instance()->loop();
        MEMORY_BARRIER();

        // Refresh display
        if (disp) {
            disp->update();
        }

        // Heartbeat serial print and optional LED (serial always runs to keep USB alive)
        static int blip_counter = 0;
        if (blip_counter++ > 100) {
            blip_counter = 0;
            Serial.println(".");

            // Optional heartbeat LED blink (disabled by default)
            if (enable_heartbeat_led) {
                digitalWrite(33, HIGH);
            }
        } else {
            // Un-blink heartbeat LED if enabled
            if (enable_heartbeat_led) {
                digitalWrite(33, LOW);
            }
        }
    }
}

void setup1()
{
    while (!READ_VOLATILE(serial_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    while (!READ_VOLATILE(interface_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    // Create audio app with memory barrier protection
    {
        auto temp_audio_app = std::make_shared<CURRENT_AUDIO_APP>();
        temp_audio_app->Setup(AudioDriver::GetSampleRate(), interface);

        // Set up callback to send full vector to core 0 via queue
        temp_audio_app->SetCallback([](const std::vector<float>& outputs) {
            if (outputs.size() >= EuclideanAudioApp::kN_Operators) {
                euclidean_data_t data;
                for (size_t i = 0; i < EuclideanAudioApp::kN_Operators; ++i) {
                    data.generators[i] = outputs[i];
                }
                // Non-blocking send to avoid audio glitches
                queue_try_add(&euclidean_queue, &data);
            }
        });

        MEMORY_BARRIER();
        audio_app = temp_audio_app;
        MEMORY_BARRIER();
    }

    // Start audio driver
    AudioDriver::Setup(audio_app->GetDriverConfig());

    WRITE_VOLATILE(core_1_ready, true);
    while (!READ_VOLATILE(core_0_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    Serial.println("Finished initialising core 1.");
}

void loop1()
{
    // Audio app parameter processing loop
    audio_app->loop();
}
