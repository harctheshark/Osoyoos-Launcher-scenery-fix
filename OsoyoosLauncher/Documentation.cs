using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Serialization;
using OsoyoosLauncher.Utility;

namespace OsoyoosLauncher
{
    public static class Documentation
    {
        private static readonly Data _data;

        public static Data Contents { get => _data; }

        static Documentation()
        {
            Assembly assembly = Assembly.GetExecutingAssembly();
            using Stream stream = assembly.GetManifestResourceStream("ToolkitLauncher.Documentation.json");
            using StreamReader reader = new(stream);
            _data = JsonSerializer.Deserialize<Data>(reader.ReadToEnd());
            _data.UpdateBaseReference();
            _data.CopyBaseValues();
        }

        public class Data
        {
            public Uri UrlBase { get; set; }

            public Dictionary<string, Toolkit> Tookits { get; init; }

            public enum HelpURL
            {
                main,
                tasks,
                programs,
                settings,
                lighting,
                cache,
                phantom,
                instances,
                h2lod
            }

            public class Toolkit
            {
                [JsonPropertyName("Base")]
                public string BaseName { get; set; } = "_base";

                [JsonIgnore]
                public Toolkit Base { get; private set; }

                public string RunPrograms { get; set; }
                public string Tasks { get; set; }
                public string Settings {get; set; }
                public string General { get; set; }
                public string Packaging { get; set; }
                public string Lighting { get; set; }
                public string Phantom { get; set; }
                public string MultiInstance { get; set; }
                public string H2LOD { get; set; }

                public string GetURL(HelpURL requestedURI)
                {
                    string url = requestedURI switch 
                    {
                        HelpURL.programs => RunPrograms,
                        HelpURL.tasks => Tasks,
                        HelpURL.settings => Settings,
                        HelpURL.lighting => Lighting,
                        HelpURL.cache => Packaging,
                        HelpURL.phantom => Phantom,
                        HelpURL.instances => MultiInstance,
                        HelpURL.h2lod => H2LOD,
                        _ => General,
                    };

                    if (string.IsNullOrWhiteSpace(url))
                    {
                        return General;
                    }
                    else 
                    {
                        return url;
                    }
                }

                public void UpdateBaseReference(Data parent)
                {
                    Base = parent.Tookits[BaseName];

                    if (Base == this) // fix circular references
                    {
                        Base = null;
                    } 
                }

                /// <summary>
                /// Use reflection to fill in any null values using the base toolkit
                /// </summary>
                public void CopyBaseValues()
                {
                    if (Base is null) 
                    {
                        return;
                    }

                    Base.CopyBaseValues();

                    PropertyInfo[] properties = typeof(Toolkit).GetProperties();

                    foreach (PropertyInfo property in properties)
                    {
                        if (property.GetValue(this) is not null) 
                        {
                            continue;
                        }

                        property.SetValue(this, property.GetValue(Base));
                    }
                }
            }

            public void UpdateBaseReference()
            {
                foreach (Toolkit toolkit in Tookits.Values)
                {
                    toolkit.UpdateBaseReference(this);
                }
            }

            public void CopyBaseValues()
            {
                foreach (Toolkit toolkit in Tookits.Values)
                {
                    toolkit.CopyBaseValues();
                }
            }

            public void OpenURL(string toolkitName, HelpURL url)
            {
                Process.OpenURL(UrlBase + Tookits[toolkitName].GetURL(url));
            }
        }
    }
}
