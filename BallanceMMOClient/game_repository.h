#pragma once
#include <BML/BMLAll.h>
class game_repository
{
	CKDataArray* current_level_array_ = nullptr;

	CK3dObject* get_current_ball() {
		if (current_level_array_)
			return static_cast<CK3dObject*>(current_level_array_->GetElementObject(0, 1));

		return nullptr;
	}
};

