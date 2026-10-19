#pragma once
#include <Windows.h>
#include <d3d12.h>
// Called before model publication so exact upstream layout witnesses see original bytes.
void TRPPreparePacing(ID3D12Device*, HMODULE) noexcept;
bool TRPPacingHasOwner() noexcept;
bool TRPPacingIsUnsafe() noexcept;
