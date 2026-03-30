using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Input;

namespace OsoyoosLauncher
{
    /// <summary>
    /// Interaction logic for AnimLengthPrompt.xaml
    /// </summary>
    public partial class AnimLengthPrompt : Window
    {
        public AnimLengthPrompt()
        {
            InitializeComponent();
        }

        private void Button_OK_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = true;
            Close();
        }

        private void Button_Cancel_Click(object sender, RoutedEventArgs e)
        {
            Close();
        }

        private void Spaces_PreviewKeyDown(object sender, KeyEventArgs e)
        {
            //Set handled to true if the key is space. Stops us from entering spaces in textboxes.
            if (e.Key == Key.Space) 
            {
                e.Handled = true;
            }
        }

        private void Numbers_Only(object sender, TextCompositionEventArgs e)
        {
            e.Handled = Regex.IsMatch(e.Text, "[^0-9]+");
        }
    }
}
