using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace OsoyoosLauncher.Utility
{
    // JMS Material Parser
    // TODO (PepperMan) - Make this less hardcoded, changes to line positions in JMS format will break this
    public class JMSMaterialReader
    {
        public static string[] ReadAllMaterials(string[] files, string tag_path, string gameType)
        {
            string line;
            List<string> shaders = new();
            // Find name of each jms file, then grab every material name from it
            foreach (string file in files)
            {
                int counter = 0;

                if (Path.GetExtension(file).ToLower() is ".jms")
                {
                    string full_jms_path = file;

                    // Open shader_collections.txt
                    List<string> collections = new();

                    string collection_path = tag_path + @"\scenarios\shaders\shader_collections.shader_collections";
                    if (gameType == "H3" || gameType == "H3ODST")
                    {
                        collection_path = tag_path + @"\levels\shader_collections.txt";
                    }
                    if (File.Exists(collection_path))
                    {
                        using (StreamReader sr = new StreamReader(collection_path))
                        {
                            // Grab every shader collection prefix
                            if (gameType == "H3" || gameType == "H3ODST")
                            {
                                while ((line = sr.ReadLine()) != null)
                                {
                                    if ((line.Contains("levels") || line.Contains("scenarios") || line.Contains("objects")) && !line.Contains("shader_collections.txt"))
                                    {
                                        if (line.Contains('\t'))
                                        {
                                            collections.Add(line.Substring(0, line.IndexOf('\t')));
                                        }
                                        else
                                        {
                                            collections.Add(line.Substring(0, line.IndexOf(' ')));
                                        }
                                    }
                                }
                            }
                            else
                            {
                                while ((line = sr.ReadLine()) != null)
                                {
                                    if ((line.Contains("scenarios") || line.Contains("objects") || line.Contains("test")) && !line.Contains('='))
                                    {
                                        if (line.Contains('\t'))
                                        {
                                            collections.Add(line.Substring(0, line.IndexOf('\t')));
                                        }
                                        else
                                        {
                                            collections.Add(line.Substring(0, line.IndexOf(' ')));
                                        }
                                    }
                                }
                            }
                        }
                    }

                    using (StreamReader sr = new StreamReader(full_jms_path))
                    {
                        // Find Materials definition header in JMS file
                        while ((line = sr.ReadLine()) != null)
                        {
                            if (line.Contains(";### MATERIALS ###"))
                            {
                                break;
                            }
                            counter++;
                        }

                        //Grab number of materials from file
                        int numMats = int.Parse(File.ReadLines(full_jms_path).Skip(counter + 1).Take(1).First());
                        // Line number of first shader name
                        int currentLine = counter + 7;

                        // Take each material name, strip symbols, add to list
                        // Typically the most "complex" materials come in the format: prefix name extra1 extra2 extra3...
                        // So if a prefix exists, check that it is a valid collection, if so ignore it as shader will be grabbed from collection,
                        // but if it isn't, it should be treated as part of the full shader name
                        string[] extras = { "lm:", "lp:", "hl:", "ds:", "pf:", "lt:", "to:", "at:", "ro:" };
                        string shaderNameStripped;
                        for (int i = 0; i < numMats; i++)
                        {
                            string[] shaderNameSections = File.ReadLines(full_jms_path).Skip(currentLine - 1).Take(1).First().Split(' ');
                            if (shaderNameSections.Length < 2)
                            {
                                shaderNameStripped = Regex.Replace(shaderNameSections[0], "[^0-9a-zA-Z_.]", string.Empty);
                                shaders.Add(shaderNameStripped);
                            }
                            else // Shader name has spaces in it
                            {
                                // Check if section before first space is a valid shader collection prefix
                                if (!collections.Contains(shaderNameSections[0]))
                                {
                                    // Shader is not in a collection, so probably just has a space in the name
                                    string shaderPrefixAndName = "";
                                    // Check if any section is an "extra" material property, if so don't include it
                                    foreach (string part in shaderNameSections)
                                    {
                                        if (!extras.Any(part.Contains))
                                        {
                                            shaderPrefixAndName += part + ' ';
                                        }
                                    }
                                    shaderNameStripped = Regex.Replace(shaderPrefixAndName, "[^0-9a-zA-Z_. ]", string.Empty).Trim();
                                    shaders.Add(shaderNameStripped);
                                }
                                // Otherwise shader is part of an existing collection, so no need to create a new tag for it
                            }
                            // Skip to next shader name
                            currentLine += 4;
                        }
                    }
                }
            }

            // Remove duplicate shader names
            shaders = shaders.Distinct().ToList();

            return shaders.ToArray();
        }
    }
}
