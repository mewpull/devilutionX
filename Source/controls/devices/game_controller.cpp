#include "controls/devices/game_controller.h"

#ifndef USE_SDL1

#include <cstddef>

#include "controls/controller_motion.h"
#include "controls/devices/joystick.h"
#include "utils/sdl_ptrs.h"
#include "utils/stubs.h"
#include "utils/log.hpp"

namespace devilution {

// Defined in SourceX/controls/plctrls.cpp
extern bool sgbControllerActive;

std::vector<GameController> *const GameController::controllers_ = new std::vector<GameController>;

ControllerButton GameController::ToControllerButton(const SDL_Event &event)
{
	switch (event.type) {
	case SDL_CONTROLLERAXISMOTION:
		switch (event.caxis.axis) {
		case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
			if (event.caxis.value < 8192) { // 25% pressed
				trigger_left_is_down_ = false;
			}
			if (event.caxis.value > 16384 && !trigger_left_is_down_) { // 50% pressed
				trigger_left_is_down_ = true;
				return ControllerButton_AXIS_TRIGGERLEFT;
			}
			return ControllerButton_NONE;
		case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
			if (event.caxis.value < 8192) { // 25% pressed
				trigger_right_is_down_ = false;
			}
			if (event.caxis.value > 16384 && !trigger_right_is_down_) { // 50% pressed
				trigger_right_is_down_ = true;
				return ControllerButton_AXIS_TRIGGERRIGHT;
			}
			return ControllerButton_NONE;
		}
		break;
	case SDL_CONTROLLERBUTTONDOWN:
	case SDL_CONTROLLERBUTTONUP:
		switch (event.cbutton.button) {
		case SDL_CONTROLLER_BUTTON_A:
			return ControllerButton_BUTTON_A;
		case SDL_CONTROLLER_BUTTON_B:
			return ControllerButton_BUTTON_B;
		case SDL_CONTROLLER_BUTTON_X:
			return ControllerButton_BUTTON_X;
		case SDL_CONTROLLER_BUTTON_Y:
			return ControllerButton_BUTTON_Y;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK:
			return ControllerButton_BUTTON_LEFTSTICK;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
			return ControllerButton_BUTTON_RIGHTSTICK;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
			return ControllerButton_BUTTON_LEFTSHOULDER;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
			return ControllerButton_BUTTON_RIGHTSHOULDER;
		case SDL_CONTROLLER_BUTTON_START:
			return ControllerButton_BUTTON_START;
		case SDL_CONTROLLER_BUTTON_BACK:
			return ControllerButton_BUTTON_BACK;
		case SDL_CONTROLLER_BUTTON_DPAD_UP:
			return ControllerButton_BUTTON_DPAD_UP;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
			return ControllerButton_BUTTON_DPAD_DOWN;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
			return ControllerButton_BUTTON_DPAD_LEFT;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
			return ControllerButton_BUTTON_DPAD_RIGHT;
		default:
			break;
		}
	default:
		break;
	}
	return ControllerButton_NONE;
}

SDL_GameControllerButton GameController::ToSdlGameControllerButton(ControllerButton button)
{
	if (button == ControllerButton_AXIS_TRIGGERLEFT || button == ControllerButton_AXIS_TRIGGERRIGHT)
		UNIMPLEMENTED();
	switch (button) {
	case ControllerButton_BUTTON_A:
		return SDL_CONTROLLER_BUTTON_A;
	case ControllerButton_BUTTON_B:
		return SDL_CONTROLLER_BUTTON_B;
	case ControllerButton_BUTTON_X:
		return SDL_CONTROLLER_BUTTON_X;
	case ControllerButton_BUTTON_Y:
		return SDL_CONTROLLER_BUTTON_Y;
	case ControllerButton_BUTTON_BACK:
		return SDL_CONTROLLER_BUTTON_BACK;
	case ControllerButton_BUTTON_START:
		return SDL_CONTROLLER_BUTTON_START;
	case ControllerButton_BUTTON_LEFTSTICK:
		return SDL_CONTROLLER_BUTTON_LEFTSTICK;
	case ControllerButton_BUTTON_RIGHTSTICK:
		return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
	case ControllerButton_BUTTON_LEFTSHOULDER:
		return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
	case ControllerButton_BUTTON_RIGHTSHOULDER:
		return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	case ControllerButton_BUTTON_DPAD_UP:
		return SDL_CONTROLLER_BUTTON_DPAD_UP;
	case ControllerButton_BUTTON_DPAD_DOWN:
		return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	case ControllerButton_BUTTON_DPAD_LEFT:
		return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	case ControllerButton_BUTTON_DPAD_RIGHT:
		return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
	default:
		return SDL_CONTROLLER_BUTTON_INVALID;
	}
}

bool GameController::IsPressed(ControllerButton button) const
{
	const SDL_GameControllerButton gcButton = ToSdlGameControllerButton(button);
	return gcButton != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(sdl_game_controller_, gcButton) != 0;
}

bool GameController::ProcessAxisMotion(const SDL_Event &event)
{
	if (event.type != SDL_CONTROLLERAXISMOTION)
		return false;
	switch (event.caxis.axis) {
	case SDL_CONTROLLER_AXIS_LEFTX:
		leftStickXUnscaled = event.caxis.value;
		leftStickNeedsScaling = true;
		break;
	case SDL_CONTROLLER_AXIS_LEFTY:
		leftStickYUnscaled = -event.caxis.value;
		leftStickNeedsScaling = true;
		break;
	case SDL_CONTROLLER_AXIS_RIGHTX:
		rightStickXUnscaled = event.caxis.value;
		rightStickNeedsScaling = true;
		break;
	case SDL_CONTROLLER_AXIS_RIGHTY:
		rightStickYUnscaled = -event.caxis.value;
		rightStickNeedsScaling = true;
		break;
	default:
		return false;
	}
	return true;
}

void GameController::Add(int joystickIndex)
{
	Log("Opening game controller for joystick at index {}", joystickIndex);
	GameController result;
	result.sdl_game_controller_ = SDL_GameControllerOpen(joystickIndex);
	if (result.sdl_game_controller_ == nullptr) {
		Log("{}", SDL_GetError());
		SDL_ClearError();
		return;
	}
	SDL_Joystick *const sdlJoystick = SDL_GameControllerGetJoystick(result.sdl_game_controller_);
	result.instance_id_ = SDL_JoystickInstanceID(sdlJoystick);
	controllers_->push_back(result);

	const SDL_JoystickGUID guid = SDL_JoystickGetGUID(sdlJoystick);
	SDLUniquePtr<char> mapping { SDL_GameControllerMappingForGUID(guid) };
	Log("Opened game controller with mapping:\n{}", mapping.get());
}

void GameController::Remove(SDL_JoystickID instanceId)
{
	Log("Removing game controller with instance id {}", instanceId);
	for (std::size_t i = 0; i < controllers_->size(); ++i) {
		const GameController &controller = (*controllers_)[i];
		if (controller.instance_id_ != instanceId)
			continue;
		controllers_->erase(controllers_->begin() + i);
		sgbControllerActive = !controllers_->empty();
		return;
	}
	Log("Game controller not found with instance id: {}", instanceId);
}

GameController *GameController::Get(SDL_JoystickID instanceId)
{
	for (auto &controller : *controllers_) {
		if (controller.instance_id_ == instanceId)
			return &controller;
	}
	return nullptr;
}

GameController *GameController::Get(const SDL_Event &event)
{
	switch (event.type) {
	case SDL_CONTROLLERAXISMOTION:
		return Get(event.caxis.which);
	case SDL_CONTROLLERBUTTONDOWN:
	case SDL_CONTROLLERBUTTONUP:
		return Get(event.jball.which);
	default:
		return nullptr;
	}
}

const std::vector<GameController> &GameController::All()
{
	return *controllers_;
}

bool GameController::IsPressedOnAnyController(ControllerButton button)
{
	for (auto &controller : *controllers_)
		if (controller.IsPressed(button))
			return true;
	return false;
}

} // namespace devilution
#endif