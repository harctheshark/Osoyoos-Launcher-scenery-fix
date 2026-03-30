using OsoyoosLauncher.Utility;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using System.Windows;

namespace OsoyoosLauncher
{
    /// <summary>
    /// Interaction logic for App.xaml
    /// </summary>
    public partial class App : Application
    {
        private static readonly string appdata_path = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        private static readonly string appdata_local_path = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        private static readonly int MAX_DELETE_RETRY = 10;
        private const string save_folder = "Osoyoos";

        public static readonly string DeleteOldCommand = "-DeleteOldInternal";

        public static string TempFolder => Path.Combine(appdata_local_path, save_folder, "Temp");

        public static string OsoyoosSavePath
        {
            get
            {
                return Path.Join(appdata_path, save_folder);
            }
        }

        private async void handleDeleteCommand(string file)
        {
            for (int i = 0; i < MAX_DELETE_RETRY; i++) 
            {
                await Task.Delay(2000); // give the parent time to exit

                try
                {
                    File.Delete(file);
                    Trace.WriteLine($"Deleted \"{file}\" on attempt {i}");
                    return;
                }
                catch
                {
                    Trace.WriteLine($"Failed to delete \"{file}\" on attempt {i}");
                }
            }
            Trace.WriteLine($"Gave up attempted to delete \"{file}\" after reaching MAX_DELETE_RETRY ({MAX_DELETE_RETRY})");
        }

        protected override void OnStartup(StartupEventArgs e)
        {
            try
            {
                Directory.CreateDirectory(TempFolder);
            } 
            catch (Exception ex)
            {
                Trace.WriteLine("Failed to create temporary folder:");
                Trace.WriteLine(ex);
            }

            Documentation.Contents.GetHashCode(); // touch

            // check startup commands
            if (e.Args.Length >= 2)
            {
                if (e.Args[0] == DeleteOldCommand) 
                {
                    handleDeleteCommand(e.Args[1]);
                }
            }

            base.OnStartup(e);

            LogManager.RotateLogs();
            ClearTemporaryFolder();

        }

        private static void ClearTemporaryFolder()
        {
            Trace.WriteLine($"Clearing old temporary files....");
            try
            {
                IEnumerable<FileInfo> files = new DirectoryInfo(TempFolder).EnumerateFiles();
                int deletedCount = 0;
                // delete selected files
                foreach (var file in files)
                {
                    try
                    {
                        file.Delete();
                        deletedCount++;
                    }
                    catch (Exception ex)
                    {
                        Trace.WriteLine($"Failed to delete old temporary file {file.FullName} \r\n {ex}");
                    }
                }

                Trace.WriteLine($"Deleted {deletedCount} temporary files.");
            }
            catch (Exception ex)
            {
                Trace.WriteLine($"Failed to enumerate temporary files {ex}");
            }
        }
    }
}
