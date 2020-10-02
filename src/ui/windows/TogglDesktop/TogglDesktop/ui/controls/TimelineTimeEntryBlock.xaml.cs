using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using TogglDesktop.ViewModels;

namespace TogglDesktop
{
    /// <summary>
    /// Interaction logic for TimelineTimeEntryBlock.xaml
    /// </summary>
    public partial class TimelineTimeEntryBlock : UserControl
    {
        public TimelineTimeEntryBlock()
        {
            InitializeComponent();
        }

        private void OnTimeEntryBlockMouseEnter(object sender, MouseEventArgs e)
        {
            if (sender is FrameworkElement uiElement && uiElement.DataContext is TimeEntryBlock curBlock)
            {
                TimeEntryPopup.VerticalOffset = uiElement.ActualHeight / 2;
            }
        }
    }
}
