#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "sys_mutex.h"

namespace vm { using namespace ps3; }

logs::channel sys_mutex("sys_mutex", logs::level::notice);

extern u64 get_system_time();

error_code sys_mutex_create(vm::ptr<u32> mutex_id, vm::ptr<sys_mutex_attribute_t> attr)
{
	sys_mutex.warning("sys_mutex_create(mutex_id=*0x%x, attr=*0x%x)", mutex_id, attr);

	if (!mutex_id || !attr)
	{
		return CELL_EFAULT;
	}

	const u32 protocol = attr->protocol;

	switch (protocol)
	{
	case SYS_SYNC_FIFO: break;
	case SYS_SYNC_PRIORITY: break;
	case SYS_SYNC_PRIORITY_INHERIT:
		sys_mutex.todo("sys_mutex_create(): SYS_SYNC_PRIORITY_INHERIT");
		break;
	default:
	{
		sys_mutex.error("sys_mutex_create(): unknown protocol (0x%x)", protocol);
		return CELL_EINVAL;
	}
	}

	const u32 recursive = attr->recursive;

	switch (recursive)
	{
	case SYS_SYNC_RECURSIVE: break;
	case SYS_SYNC_NOT_RECURSIVE: break;	
	default:
	{
		sys_mutex.error("sys_mutex_create(): unknown recursive (0x%x)", recursive);
		return CELL_EINVAL;
	}
	}

	if (attr->pshared != SYS_SYNC_NOT_PROCESS_SHARED || attr->adaptive != SYS_SYNC_NOT_ADAPTIVE || attr->ipc_key || attr->flags)
	{
		sys_mutex.error("sys_mutex_create(): unknown attributes (pshared=0x%x, adaptive=0x%x, ipc_key=0x%llx, flags=0x%x)", attr->pshared, attr->adaptive, attr->ipc_key, attr->flags);
		return CELL_EINVAL;
	}

	if (const u32 id = idm::make<lv2_obj, lv2_mutex>(protocol, recursive, attr->name_u64))
	{
		*mutex_id = id;
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code sys_mutex_destroy(u32 mutex_id)
{
	sys_mutex.warning("sys_mutex_destroy(mutex_id=0x%x)", mutex_id);

	const auto mutex = idm::withdraw<lv2_obj, lv2_mutex>(mutex_id, [](lv2_mutex& mutex) -> CellError
	{
		if (mutex.owner || mutex.lock_count)
		{
			return CELL_EBUSY;
		}

		if (mutex.cond_count)
		{
			return CELL_EPERM;
		}

		return {};
	});

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	if (mutex.ret)
	{
		return mutex.ret;
	}

	return CELL_OK;
}

error_code sys_mutex_lock(ppu_thread& ppu, u32 mutex_id, u64 timeout)
{
	sys_mutex.trace("sys_mutex_lock(mutex_id=0x%x, timeout=0x%llx)", mutex_id, timeout);

	const auto mutex = idm::get<lv2_obj, lv2_mutex>(mutex_id, [&](lv2_mutex& mutex)
	{
		CellError result = mutex.try_lock(ppu.id);

		if (result == CELL_EBUSY)
		{
			semaphore_lock lock(mutex.mutex);

			if (mutex.try_own(ppu, ppu.id))
			{
				result = {};
			}
			else
			{
				mutex.sleep(ppu, timeout);
			}
		}

		return result;
	});

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	if (mutex.ret)
	{
		if (mutex.ret != CELL_EBUSY)
		{
			return mutex.ret;
		}
	}
	else
	{
		return CELL_OK;
	}

	ppu.gpr[3] = CELL_OK;

	while (!ppu.state.test_and_reset(cpu_flag::signal))
	{
		if (timeout)
		{
			const u64 passed = get_system_time() - ppu.start_time;

			if (passed >= timeout)
			{
				semaphore_lock lock(mutex->mutex);

				if (!mutex->unqueue(mutex->sq, &ppu))
				{
					timeout = 0;
					continue;
				}

				ppu.gpr[3] = CELL_ETIMEDOUT;
				break;
			}

			thread_ctrl::wait_for(timeout - passed);
		}
		else
		{
			thread_ctrl::wait();
		}
	}

	ppu.test_state();
	return not_an_error(ppu.gpr[3]);
}

error_code sys_mutex_trylock(ppu_thread& ppu, u32 mutex_id)
{
	sys_mutex.trace("sys_mutex_trylock(mutex_id=0x%x)", mutex_id);

	const auto mutex = idm::check<lv2_obj, lv2_mutex>(mutex_id, [&](lv2_mutex& mutex)
	{
		return mutex.try_lock(ppu.id);
	});

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	if (mutex.ret)
	{
		if (mutex.ret == CELL_EBUSY)
		{
			return not_an_error(CELL_EBUSY);
		}

		return mutex.ret;
	}

	return CELL_OK;
}

error_code sys_mutex_unlock(ppu_thread& ppu, u32 mutex_id)
{
	sys_mutex.trace("sys_mutex_unlock(mutex_id=0x%x)", mutex_id);

	const auto mutex = idm::check<lv2_obj, lv2_mutex>(mutex_id, [&](lv2_mutex& mutex)
	{
		return mutex.try_unlock(ppu.id);
	});

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	if (mutex.ret == CELL_EBUSY)
	{
		semaphore_lock lock(mutex->mutex);

		if (auto cpu = mutex->reown<ppu_thread>())
		{
			ppu.state += cpu_flag::is_waiting;
			mutex->awake(*cpu);
		}
	}
	else if (mutex.ret)
	{
		return mutex.ret;
	}

	ppu.test_state();
	return CELL_OK;
}
