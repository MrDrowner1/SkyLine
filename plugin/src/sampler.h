#pragma once

// Which DLL a thread is running in, from a copy of its stack taken while it is suspended (no allocation or locks then).
namespace Sampler
{
	void SetSelf(HMODULE a_self);
	void RefreshModules();
	// SKSE plugin whose own code made the current call on this thread, "" for the game. Safe from any thread.
	// a_chain gets the modules on the stack, innermost first, e.g. "game > OpenAnimationReplacer.dll > game x3".
	std::string Owner(std::string* a_chain = nullptr);

	class Thread
	{
	public:
		~Thread();
		void  Open(DWORD a_id);
		DWORD Id() const { return _id; }
		// "" = could not sample, else e.g. "game code", "SKSE", "OpenAnimationReplacer.dll".
		std::string Sample();

	private:
		HANDLE         _handle{ nullptr };
		DWORD          _id{ 0 };
		std::uintptr_t _stackBase{ 0 };
	};
}
