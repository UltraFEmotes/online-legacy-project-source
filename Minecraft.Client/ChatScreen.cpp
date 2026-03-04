#include "stdafx.h"
#include "ChatScreen.h"
#include "MultiplayerLocalPlayer.h"
#include "..\Minecraft.World\SharedConstants.h"
#include "..\Minecraft.World\StringHelpers.h"

const wstring ChatScreen::allowedChars = SharedConstants::acceptableLetters;

ChatScreen::ChatScreen()
{
	frame = 0;
	wasKeyboardRequested = false;
}

void ChatScreen::init()
{
	Keyboard::enableRepeatEvents(true);
}

void ChatScreen::removed()
{
	Keyboard::enableRepeatEvents(false);
}

void ChatScreen::tick()
{
	frame++;
	if (frame == 1 && !wasKeyboardRequested)
	{
		wasKeyboardRequested = true;
		if (minecraft && minecraft->player)
		{
			// Give the player their native input window
			InputManager.RequestKeyboard(L"Enter Chat Message", L"", (DWORD)0, SharedConstants::maxChatLength, &ChatScreen::KeyboardCompleteCallback, this, C_4JInput::EKeyboardMode_Default);
		}
	}
}

int ChatScreen::KeyboardCompleteCallback(LPVOID lpParam, const bool bRes)
{
	ChatScreen* pScreen = (ChatScreen*)lpParam;
	if (bRes && pScreen->minecraft && pScreen->minecraft->player)
	{
		uint16_t pchText[256];
		ZeroMemory(pchText, 256 * sizeof(uint16_t));
		InputManager.GetText(pchText);

		wstring msg = (wchar_t*)pchText;
		wstring trim = trimString(msg);
		if (trim.length() > 0)
		{
			if (!pScreen->minecraft->handleClientSideCommand(trim))
			{
				wstring username = pScreen->minecraft->player->getName();
				wstring formattedMessage = L"<" + username + L"> " + trim;
				pScreen->minecraft->player->chat(formattedMessage);
			}
		}
	}

	if (pScreen->minecraft)
	{
		pScreen->minecraft->setScreen(NULL);
	}
	return 0;
}

void ChatScreen::keyPressed(wchar_t ch, int eventKey)
{
    if (eventKey == Keyboard::KEY_ESCAPE)
	{
        minecraft->setScreen(NULL);
        return;
    }
}

void ChatScreen::render(int xm, int ym, float a)
{
    fill(2, height - 14, width - 2, height - 2, 0x80000000);
    drawString(font, L"> " + message + (frame / 6 % 2 == 0 ? L"_" : L""), 4, height - 12, 0xe0e0e0);

    Screen::render(xm, ym, a);
}

void ChatScreen::mouseClicked(int x, int y, int buttonNum)
{
    if (buttonNum == 0)
	{
        if (minecraft->gui->selectedName != L"")	// 4J - was NULL comparison
		{
			if (message.length() > 0 && message[message.length()-1]!=L' ')
			{
                message += L" ";
            }
            message += minecraft->gui->selectedName;
            unsigned int maxLength = SharedConstants::maxChatLength;
            if (message.length() > maxLength)
			{
                message = message.substr(0, maxLength);
            }
        }
		else
		{
            Screen::mouseClicked(x, y, buttonNum);
        }
    }

}