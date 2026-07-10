using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Threading.Tasks;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.System.Memory;

namespace OsoyoosLauncher.Utility
{
	internal class H2ToolLightmapFixInjector : IProcessInjector
	{
		private static readonly Guid _uuid = Guid.Parse("1B6E9E6C-DBA1-47DA-A7B4-7B940808FCB2");

		public struct NopFill
		{
			public NopFill(uint offset, uint length)
			{
				Offset = offset;
				Length = length;
			}

			public uint Offset;
			public uint Length;
		}

		// A self-contained, RELOCATABLE code cave to map into the target and jump into. Written via
		// VirtualAllocEx (any address) + WriteProcessMemory while the process is suspended, so it can't race
		// (unlike DLL/CreateRemoteThread injection, which drops most workers at high instance counts).
		//   Image        = mapped PE image built at PreferredBase
		//   PreferredBase = link base of Image (relocations are relative to this)
		//   RelocRVAs    = HIGHLOW base-relocation RVAs; only entries whose stored value lands inside the cave
		//                  image are rebased (tool addresses baked into the cave are left untouched)
		//   Jumps        = (tool entry to patch, cave hook RVA) — patched with a jmp to allocBase+hookRVA
		//   Jumps        = (tool entry to patch, cave hook RVA, stolen-prologue length) — the entry is overwritten
		//                  with a 5-byte jmp to allocBase+hookRVA plus (stealLen-5) nops, so the stub replays exactly
		//                  the stolen instructions. stealLen varies per hook (8 for most; 6 for sub_4A923A whose
		//                  aligned prologue's next instruction begins at byte 6).
		public sealed record CavePayload(byte[] Image, uint PreferredBase, uint[] RelocRVAs, (uint patchVA, uint hookRVA, uint stealLen)[] Jumps);

		private readonly uint _baseAddress;
		private readonly IEnumerable<NopFill> _nopfills;
		private readonly CavePayload _cave;
		public IProcessInjector DaisyChainedInjector { get; set; } = null;

		public H2ToolLightmapFixInjector(uint baseAddress, IEnumerable<NopFill> nopFills, CavePayload cave = null, IProcessInjector daisyChain = null)
		{
			_baseAddress = baseAddress;
			_nopfills = nopFills;
			_cave = cave;
			DaisyChainedInjector = daisyChain;
		}

		public Guid SetupEnviroment(ProcessStartInfo startInfo)
		{
			if (DaisyChainedInjector is null)
			{
				return _uuid;
			}
			else
			{
				return DaisyChainedInjector.SetupEnviroment(startInfo);
			}
		}

		[SupportedOSPlatform("windows5.1.2600")]
		[DllImport("ntdll.dll", PreserveSig = false)]
		public static extern void NtSuspendProcess(IntPtr processHandle);

		[SupportedOSPlatform("windows5.1.2600")]
		[DllImport("ntdll.dll", PreserveSig = false)]
		public static extern void NtResumeProcess(IntPtr processHandle);

		// When a cave is present we MUST patch before the tool executes any lightmap code (the cave hooks run
		// during geometry import). Request CREATE_SUSPENDED so the patch is applied before the first instruction,
		// regardless of instance count / CPU contention. (Without this, under 32-way load the tool races past
		// geometry import before the injector's suspend lands, so only single-instance bakes got the fix.)
		public virtual bool ShouldSuspendOnLaunch => _cave is not null || (DaisyChainedInjector is not null && DaisyChainedInjector.ShouldSuspendOnLaunch);

		[SupportedOSPlatform("windows5.1.2600")]
		public async Task<bool> Inject(Guid id, System.Diagnostics.Process process)
		{
			if (DaisyChainedInjector is null)
				Debug.Assert(id == _uuid);

			bool success = true;
			if (DaisyChainedInjector is not null)
			{
				success = await DaisyChainedInjector.Inject(id, process);
				Trace.WriteLine($"[H2 LM Patcher] Daisy chained injector done, succes = {success}");
			}

			// If we requested CREATE_SUSPENDED (cave present), the process is already suspended (count 1) and the
			// framework does NOT resume it — our finally NtResumeProcess is what starts it. Adding our own suspend
			// would leave it suspended forever, so only self-suspend when the process is already running.
			bool alreadySuspendedOnLaunch = ShouldSuspendOnLaunch;

			// use try-finally to ensure the process is always resumed no matter whatever the patching was sucessful or not
			try
			{
				if (!alreadySuspendedOnLaunch)
					NtSuspendProcess(process.Handle);
				Trace.WriteLine($"[H2 LM Patcher] Target process ready for patching (early={alreadySuspendedOnLaunch}, last error = {Marshal.GetLastWin32Error()})");

				// Do NOT read process.MainModule: when created suspended the loader hasn't run and the module
				// list is empty, so MainModule throws. Tool is no-ASLR (MD5-gated), so base == _baseAddress.
				IntPtr moduleBase = (IntPtr)(nint)_baseAddress;
				Trace.WriteLine($"[H2 LM Patcher] Patch target base (assumed no-ASLR): {moduleBase:X}");

				foreach (NopFill fill in _nopfills)
				{
					IntPtr translatedAddress = IntPtr.Add(moduleBase, (int)(fill.Offset - _baseAddress));
					Trace.WriteLine($"Nopfilling {fill.Length} bytes at {fill.Offset:X} (translated address => {translatedAddress:X})");

					byte[] nopValues = new byte[fill.Length];
					Array.Fill<byte>(nopValues, 0x90);

					bool writeSuccess;

					unsafe
					{
						fixed (byte* nopVals = nopValues)
							writeSuccess = PInvoke.WriteProcessMemory((HANDLE)process.Handle, translatedAddress.ToPointer(), nopVals, (nuint)nopValues.Length, null);
					}

					if (!writeSuccess)
					{
						Trace.WriteLine("Failed to write patch to memory!");
						success = false;
					}
				}

				// Map + hook the self-contained scenery-fix cave. Best-effort: a cave problem is logged but does
				// NOT fail the overall injection (the tag-overwrite nopfills above are what the worker-0 delay
				// logic keys off, and they must not be held hostage to the cave).
				if (_cave is not null && true)
				{
					try
					{
						unsafe
						{
							// allocate ANYWHERE (relocatable) so we never depend on a specific address being free
							void* remote = PInvoke.VirtualAllocEx((HANDLE)process.Handle, null,
								(nuint)_cave.Image.Length,
								VIRTUAL_ALLOCATION_TYPE.MEM_COMMIT | VIRTUAL_ALLOCATION_TYPE.MEM_RESERVE,
								PAGE_PROTECTION_FLAGS.PAGE_EXECUTE_READWRITE);

							if (remote is null)
							{
								Trace.WriteLine($"[H2 LM Patcher] cave VirtualAllocEx failed (err {Marshal.GetLastWin32Error()})");
							}
							else
							{
								uint allocBase = (uint)remote;
								uint delta = allocBase - _cave.PreferredBase;
								uint caveLo = _cave.PreferredBase, caveHi = _cave.PreferredBase + (uint)_cave.Image.Length;

								// rebase a private copy: only fix values that point inside the cave image
								byte[] buf = (byte[])_cave.Image.Clone();
								foreach (uint rva in _cave.RelocRVAs)
								{
									uint v = BitConverter.ToUInt32(buf, (int)rva);
									if (v >= caveLo && v < caveHi)
										BitConverter.GetBytes(v + delta).CopyTo(buf, (int)rva);
								}

								bool writeOk;
								fixed (byte* img = buf)
									writeOk = PInvoke.WriteProcessMemory((HANDLE)process.Handle, remote, img, (nuint)buf.Length, null);

								foreach ((uint patchVA, uint hookRVA, uint stealLen) in _cave.Jumps)
								{
									uint target = allocBase + hookRVA;
									byte[] patch = new byte[stealLen];
									patch[0] = 0xE9; // jmp rel32
									BitConverter.GetBytes((int)(target - (patchVA + 5))).CopyTo(patch, 1);
									for (uint bi = 5; bi < stealLen; bi++) patch[bi] = 0x90; // nop-pad remaining stolen bytes
									IntPtr addr = IntPtr.Add(moduleBase, (int)(patchVA - _baseAddress));
									fixed (byte* p = patch)
										writeOk &= PInvoke.WriteProcessMemory((HANDLE)process.Handle, addr.ToPointer(), p, (nuint)patch.Length, null);
								}

								Trace.WriteLine($"[H2 LM Patcher] scenery fix cave applied @ {allocBase:X} (write ok = {writeOk})");
							}
						}
					}
					catch (Exception cex)
					{
						Trace.WriteLine($"[H2 LM Patcher] cave apply threw (ignored): {cex.Message}");
					}
				}

				Trace.WriteLine($"[H2 LM Patcher] Done patching, success = {success}");

				return success;
			} catch(Exception ex)
			{
				Trace.WriteLine($"[H2 LM Patcher] Unexpected expection, bailing out: {ex}");

				return false;
			} finally
			{
				NtResumeProcess(process.Handle);
				Trace.WriteLine($"[H2 LM Patcher] Process resumed, all done (last error = {Marshal.GetLastWin32Error()})");
			}

		}
	}
}
