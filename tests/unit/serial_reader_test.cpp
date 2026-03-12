#include "../src/serial_reader.hpp"
#include "../tests/test_helpers.hpp"
#include <cstring>

// ---------------------------------------------------------------------------
// serial_prompts::prompt_response — prompt detection and reply selection
// ---------------------------------------------------------------------------

// Lines ending with ':' after trimming are prompts.
void test_prompt_response_generic_colon() {
    // A bare prompt with no recognised keyword → bare CR (accept default).
    std::string line = "Min charge current <0.15>:";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r != nullptr, "Generic ':' prompt is recognised");
    ASSERT(strcmp(r, "\r") == 0, "Generic prompt returns bare CR");
}

void test_prompt_response_generic_question() {
    // A prompt ending with '?' → bare CR.
    std::string line = "Enable something <Y>?";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r != nullptr, "Generic '?' prompt is recognised");
    ASSERT(strcmp(r, "\r") == 0, "Generic '?' prompt returns bare CR");
}

void test_prompt_response_max_charge_current() {
    std::string line = "Max charge current <1>:";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r != nullptr, "Max charge current prompt recognised");
    ASSERT(strcmp(r, "10\r") == 0, "Max charge current → '10\\r'");
}

void test_prompt_response_max_charge_current_trailing_spaces() {
    // Real device output sometimes has trailing spaces before silence.
    std::string line = "Max charge current <1>:   ";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r != nullptr, "Max charge current with trailing spaces recognised");
    ASSERT(strcmp(r, "10\r") == 0, "Trailing spaces don't break keyword match");
}

void test_prompt_response_reset_to_default() {
    std::string line = "Reset to default values <N>?";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r != nullptr, "Reset to default prompt recognised");
    ASSERT(strcmp(r, "N\r") == 0, "Reset to default → 'N\\r'");
}

void test_prompt_response_battery_save_mode() {
    std::string line = "Battery Save Mode <N>?";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r != nullptr, "Battery Save Mode prompt recognised");
    ASSERT(strcmp(r, "N\r") == 0, "Battery Save Mode → 'N\\r'");
}

// Battery type must NOT return an explicit "4\r" — doing so causes a device
// soft-reset even when the value already matches the default.
void test_prompt_response_battery_type_accepts_default() {
    std::string line = "Battery type <4>:";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r != nullptr, "Battery type prompt recognised");
    ASSERT(strcmp(r, "\r") == 0,
           "Battery type returns bare CR (not '4\\r') to avoid device reset");
}

// Informational streaming lines that happen to contain ':' must NOT be treated
// as prompts.
void test_prompt_response_streaming_line_not_prompt() {
    // Streaming data line — ends with '.' not ':' or '?'
    std::string line = "PS=14.01 Sol=0.00 Bat=13.25V, 5.36A Min=20 P=800 adc=600";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r == nullptr, "Streaming data line not treated as prompt");
}

void test_prompt_response_jumpers_line_not_prompt() {
    // "Jumpers: Li / 1A  On for 3631 Minutes." — ends with '.' not ':'
    std::string line = "Jumpers: Li / 1A  On for 3631 Minutes.";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r == nullptr, "Jumpers informational line not treated as prompt");
}

void test_prompt_response_empty_not_prompt() {
    const char* r = serial_prompts::prompt_response("");
    ASSERT(r == nullptr, "Empty string not treated as prompt");
}

void test_prompt_response_whitespace_only_not_prompt() {
    const char* r = serial_prompts::prompt_response("   \t  ");
    ASSERT(r == nullptr, "Whitespace-only string not treated as prompt");
}

// A line that *looks* like it ends with ':' but only because it's a field
// label in the streaming output should be caught by content, not suffix alone.
// (This exercises that prompt_response checks ':'/? suffix correctly.)
void test_prompt_response_targetv_line_not_prompt() {
    // TargetV= line ends in a digit — not a prompt.
    std::string line = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    const char* r = serial_prompts::prompt_response(line);
    ASSERT(r == nullptr, "TargetV streaming line not treated as prompt");
}

// ---------------------------------------------------------------------------
// serial_prompts::is_unterminated_prompt — partial buffer heuristic
// ---------------------------------------------------------------------------

// Real 'S'-menu prompts look like: "Max charge current <1>:"  (no trailing \n)
void test_unterminated_prompt_colon_after_angle_bracket() {
    ASSERT(serial_prompts::is_unterminated_prompt("Max charge current <1>:"),
           "Partial buffer ending with '>:' is an unterminated prompt");
}

void test_unterminated_prompt_question_after_angle_bracket() {
    ASSERT(serial_prompts::is_unterminated_prompt("Reset to default values <N>?"),
           "Partial buffer ending with '>?' is an unterminated prompt");
}

void test_unterminated_prompt_with_trailing_space() {
    // Some devices append a space after the colon before stopping.
    ASSERT(serial_prompts::is_unterminated_prompt("Max charge current <1>: "),
           "Trailing space does not defeat '>:' detection");
}

// "Jumpers: Li / 1A  On for 3631 Minutes." — has ':' but not preceded by '>'
void test_unterminated_prompt_jumpers_line_rejected() {
    ASSERT(!serial_prompts::is_unterminated_prompt(
               "Jumpers: Li / 1A  On for 3631 Minutes."),
           "Jumpers informational line rejected: no '>' before final char");
}

// A partial streaming line mid-accumulation — no match.
void test_unterminated_prompt_partial_streaming_rejected() {
    ASSERT(!serial_prompts::is_unterminated_prompt("PS=14.01 Sol=0.00"),
           "Partial streaming line not an unterminated prompt");
}

// Empty / very short buffers must not crash or falsely match.
void test_unterminated_prompt_empty_rejected() {
    ASSERT(!serial_prompts::is_unterminated_prompt(""), "Empty buffer rejected");
}

void test_unterminated_prompt_single_char_rejected() {
    ASSERT(!serial_prompts::is_unterminated_prompt(":"), "Single ':' rejected");
}

void test_unterminated_prompt_two_chars_no_angle_bracket() {
    ASSERT(!serial_prompts::is_unterminated_prompt("x:"), "x: rejected (no '>')");
}

// "Battery:" partial — ends with ':' but prev char is 'y', not '>'
void test_unterminated_prompt_battery_partial_rejected() {
    ASSERT(!serial_prompts::is_unterminated_prompt("Battery:"),
           "'Battery:' partial rejected: char before ':' is not '>'");
}

// ---------------------------------------------------------------------------
// serial_prompts::needs_reconfigure — target current threshold
// ---------------------------------------------------------------------------

void test_needs_reconfigure_default_1a() {
    ASSERT(serial_prompts::needs_reconfigure(1.0),
           "1.0 A (jumper default) triggers reconfigure");
}

void test_needs_reconfigure_below_threshold() {
    ASSERT(serial_prompts::needs_reconfigure(5.0),
           "5.0 A (mid-range) still below 9.9 A threshold");
}

void test_needs_reconfigure_just_below_threshold() {
    ASSERT(serial_prompts::needs_reconfigure(9.8),
           "9.8 A is below 9.9 threshold");
}

void test_needs_reconfigure_at_threshold() {
    ASSERT(!serial_prompts::needs_reconfigure(9.9),
           "9.9 A is not below threshold (already configured)");
}

void test_needs_reconfigure_at_10a() {
    ASSERT(!serial_prompts::needs_reconfigure(10.0),
           "10.0 A (fully configured) does not trigger reconfigure");
}

void test_needs_reconfigure_above_10a() {
    ASSERT(!serial_prompts::needs_reconfigure(12.0),
           "12.0 A does not trigger reconfigure");
}
