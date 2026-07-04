#pragma once
#include <windows.h>
#include <string>
#include <vector>

enum class ButtonStyle  { Pill, Icon };
enum class ButtonAction { Url, Shortcut, Command };

struct Button {
    std::wstring id;
    ButtonStyle  style  = ButtonStyle::Pill;
    std::wstring label;
    std::wstring iconPath;
    ButtonAction action = ButtonAction::Url;
    std::wstring target;
};

// Owns the automation-button config (CLAUDE.md: Launcher component, Stage 5).
// Minimum-functionality config is a line-based text file (hard rule 2 — no JSON
// dependency); one button per line: style|label|action|target|iconPath(optional).
// Blank lines and lines starting with '#' or ';' are ignored. Malformed lines are
// skipped with an OutputDebugStringW note; a missing file yields zero buttons.
class Launcher
{
public:
    void Load();
    const std::vector<Button>& Buttons() const { return m_buttons; }

    static std::wstring ConfigPath();

private:
    std::vector<Button> m_buttons;
};
