using ManagedBlamHelper;
using OsoyoosLauncher.Utility;
using System;
using System.Runtime.CompilerServices;

namespace OsoyoosLauncher
{
    public class Program
    {
        [STAThread]
        [MethodImpl(MethodImplOptions.NoInlining | MethodImplOptions.NoOptimization)]
        public static int Main(string[] args)
        {
            int return_code = 0;
            // check if we are called by ourselves
            if (args.Length > 0 && args[0] == MBHandler.command_id && OperatingSystem.IsWindows())
            {
                LogManager.InitializeLogging("ManagedHelper");
                return_code = MBHandler.Premain(args[1..]);
            }
            else // otherwise just run the launcher
            {
                // run WPF application
                LogManager.InitializeLogging("Launcher");
                ApplicationMain();
            }


            return return_code;
        }

        [MethodImpl(MethodImplOptions.NoInlining | MethodImplOptions.NoOptimization)]
        public static void ApplicationMain()
        {
            var app = new App();
            app.InitializeComponent();
            app.Run();
        }
    }
}
