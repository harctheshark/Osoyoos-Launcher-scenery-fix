using System;
using System.IO;
using System.Windows.Forms;
using OsoyoosLauncher.OsoyoosInterface;

public class FilePicker
{
    private readonly OpenFileDialog fileDialog;
    private readonly FolderBrowserDialog folderDialog;
    private readonly System.Windows.Controls.TextBox textBox;
    private readonly ToolkitBase toolkitInterface;
    private readonly Options options;
    private readonly System.Windows.Controls.ListBox listBox;

#nullable enable
    public class Options
    {
        public string? title;
        public string? filter;
        public PathRoot pathRoot;
        public bool parent;
        public bool strip_extension;

        public bool IsFolderSelect() { return filter is null; }

        public enum PathRoot
        {
			FileSystem,
			Tag,
			Data,
			Tag_Data
        }

		public static Options FileSelect(string title, string filter, PathRoot pathRoot, bool parent = false, bool strip_extension = true)
        {
            Options opt = new()
            {
                title = title,
                filter = filter,
                pathRoot = pathRoot,
                parent = parent,
                strip_extension = strip_extension
            };

            return opt;
		}

		public static Options FolderSelect(string title, PathRoot pathRoot, bool parent = false, bool strip_extension = true)
		{
            Options opt = new()
            {
                title = title,
                filter = null,
                pathRoot = pathRoot,
                parent = parent,
                strip_extension = strip_extension
            };

            return opt;
		}
	}
#nullable restore

	public FilePicker(System.Windows.Controls.TextBox box, ToolkitBase toolkit, Options options, string InitialDirectory)
	{
		if (toolkit is not null && !Path.IsPathRooted(InitialDirectory)) 
		{
            InitialDirectory = Path.Join(toolkit.BaseDirectory, InitialDirectory);
        }

		textBox = box;
		toolkitInterface = toolkit;

		this.options = options;

		if (options.IsFolderSelect()) 
		{
            folderDialog = new FolderBrowserDialog
            {
                Description = options.title,
                SelectedPath = InitialDirectory
            };
        } 
		else 
		{
            fileDialog = new OpenFileDialog
            {
                Title = options.title,
                Filter = options.filter,
                InitialDirectory = InitialDirectory
            };
        }
	}

    public FilePicker(System.Windows.Controls.ListBox box, ToolkitBase toolkit, Options options, string InitialDirectory)
    {
		if (toolkit is not null && !Path.IsPathRooted(InitialDirectory)) 
		{
            InitialDirectory = Path.Join(toolkit.BaseDirectory, InitialDirectory);
        }

		listBox = box;
        toolkitInterface = toolkit;

        this.options = options;

        fileDialog = new OpenFileDialog
        {
            Title = options.title,
            Filter = options.filter,
            InitialDirectory = InitialDirectory,
            Multiselect = true
        };

    }

	public bool Prompt()
	{
		if (folderDialog is null && fileDialog is null) 
		{
            throw new InvalidOperationException("No valid dialog");
        }

		if (folderDialog is not null && folderDialog.ShowDialog() == DialogResult.OK)
        {
			return ProcessInput(folderDialog.SelectedPath);
		}
		if (fileDialog is not null && fileDialog.ShowDialog() == DialogResult.OK)
		{
			if (fileDialog.Multiselect)
			{
				return ProcessInput(fileDialog.FileNames);
			}
			else
			{
                return ProcessInput(fileDialog.FileName);
            }			
		}

		return false;
    }

#nullable enable

	bool ProcessInput(string path)
    {
		string? local_path = ConvertFSPathToLocalPath(path, options.pathRoot);

		if (local_path is null)
        {
			MessageBox.Show("File path was not within the current toolkit directory", "Error!");
			return false;
		}

		if (options.strip_extension) 
		{
            local_path = local_path.Substring(0, local_path.Length - Path.GetExtension(local_path).Length);
        }

		if (options.parent) 
		{
            local_path = local_path.Substring(0, local_path.Length - Path.GetFileName(local_path).Length);
        }

		textBox.Text = local_path;

		return true;
	}

    bool ProcessInput(string[] paths)
    {
		for (int i = 0; i < paths.Length; i++)
		{
            string? local_path = ConvertFSPathToLocalPath(paths[i], options.pathRoot);

            if (local_path is null)
            {
                MessageBox.Show("File path \"" + paths[i] + "\" was not within the current toolkit directory", "Error!");
                return false;
            }

			if (options.strip_extension) 
			{
                local_path = local_path.Substring(0, local_path.Length - Path.GetExtension(local_path).Length);
            }

			if (options.parent) 
			{
                local_path = local_path.Substring(0, local_path.Length - Path.GetFileName(local_path).Length);
            }

			listBox.Items.Add(local_path);
        }

        return true;
    }

    /// <summary>
    ///
    /// </summary>
    /// <param name="path"></param>
    /// <param name="root"></param>
    /// <returns></returns>
    string? ConvertFSPathToLocalPath(string path, Options.PathRoot root)
	{
		if (root == Options.PathRoot.FileSystem) 
		{
            return Path.GetFullPath(path);
        }

		string base_path;

		switch (root)
        {
			case Options.PathRoot.Data:
				base_path = toolkitInterface.GetDataDirectory();
				break;
			case Options.PathRoot.Tag:
				base_path = toolkitInterface.GetTagDirectory();
				break;
			case Options.PathRoot.Tag_Data:
				base_path = toolkitInterface.GetDataDirectory();
				if (path.StartsWith(toolkitInterface.GetTagDirectory())) 
				{
                    base_path = toolkitInterface.GetTagDirectory();
                }
				break;
			default:
				throw new InvalidOperationException();
		}

		if (!path.StartsWith(base_path)) 
		{
            return null;
        }

		return Path.GetRelativePath(base_path, path);
	}

#nullable restore
}
