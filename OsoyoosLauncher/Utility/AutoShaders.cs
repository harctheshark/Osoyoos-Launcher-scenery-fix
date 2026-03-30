using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace OsoyoosLauncher.Utility
{
    public static class AutoShaders
    {
        public static async Task<bool> CreateEmptyShaders(string tag_path, string data_path, string path, string gameType)
        {
            //Grabbing full path from drive letter to render folder
            string jmsPath = Path.Join(data_path, path, "render");

            // Get all files in render folder
            string[] files = null;
            try
            {
                files = Directory.GetFiles(jmsPath);
            }
            catch (DirectoryNotFoundException)
            {
                MessageBox.Show("Unable to find JMS filepath!\nThis usually happens if your filepath contains invalid characters.\nAborting model import and shader generation...", "Fatal Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }

            string destinationShadersFolder = Path.Join(tag_path, path, "shaders");

            // Checking if shaders already exist, if so don't re-gen them
            try
            {
                if (!(Directory.GetFiles(destinationShadersFolder) == Array.Empty<string>()))
                {
                    Trace.WriteLine("Shaders already exist!");
                    if (MessageBox.Show("Shaders for this model already exist!\nWould you like to generate any missing shaders?", "Shader Gen. Warning", MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
                    {
                        string[] shaders = JMSMaterialReader.ReadAllMaterials(files, tag_path, gameType);
                        await shaderGen(shaders, destinationShadersFolder, tag_path, gameType);
                    }
                    else
                    {
                        return true;
                    }
                }
                else
                {
                    string[] shaders = JMSMaterialReader.ReadAllMaterials(files, tag_path, gameType);
                    await shaderGen(shaders, destinationShadersFolder, tag_path, gameType);
                }
            }
            catch (DirectoryNotFoundException)
            {
                Trace.WriteLine("No folders exist, proceeding with shader gen");
                string[] shaders = JMSMaterialReader.ReadAllMaterials(files, tag_path, gameType);
                await shaderGen(shaders, destinationShadersFolder, tag_path, gameType);
            }

            static async Task<bool> shaderGen(string[] shaders, string destinationShadersFolder, string tagFolder, string gameType)
            {
                // Create directories               
                Directory.CreateDirectory(destinationShadersFolder);

                // Make sure default.shader exists, if not, create it
                string defaultShaderLocation = gameType == "H2"
                    ? Path.Join(tagFolder, @"\shaders\default.shader")
                    : Path.Join(tagFolder, @"\levels\shared\shaders\simple\default.shader");

                byte[] default_shader_contents = null;

                try
                {
                    default_shader_contents = await File.ReadAllBytesAsync(defaultShaderLocation);
                }
                catch
                {
                    Trace.WriteLine("Default shader missing, writing to disk!");
                    switch (gameType)
                    {
                        case "H3":
                            default_shader_contents = OsoyoosLauncher.Utility.Resources.defaultH3;
                            break;
                        case "H3ODST":
                            default_shader_contents = OsoyoosLauncher.Utility.Resources.defaultODST;
                            break;
                        case "H2":
                            default_shader_contents = OsoyoosLauncher.Utility.Resources.defaultH2;
                            break;
                    }

                    try
                    {
                        File.WriteAllBytes(defaultShaderLocation, default_shader_contents);
                    }
                    catch
                    {
                        Trace.WriteLine("Failed to write default shader, continuing anyways");
                    }
                }

                bool success = true;
                // Write each shader
                foreach (string shader in shaders)
                {
                    // skip invalid shaders
                    if (string.IsNullOrEmpty(shader))
                        continue;

                    string shader_file_path = Path.Join(destinationShadersFolder, shader + ".shader");
                    try
                    {

                        await File.WriteAllBytesAsync(shader_file_path, default_shader_contents);
                    }
                    catch
                    {
                        success = false;
                    }

                }

                return success;
            }

            // Default fall-through
            return true;
        }
    }
}
