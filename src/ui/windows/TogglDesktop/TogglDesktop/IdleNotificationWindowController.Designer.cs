﻿namespace TogglDesktop
{
    partial class IdleNotificationWindowController
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(IdleNotificationWindowController));
            this.labelIdleSince = new System.Windows.Forms.Label();
            this.labelIdleDuration = new System.Windows.Forms.Label();
            this.buttonKeepTime = new System.Windows.Forms.Button();
            this.buttonDiscardTime = new System.Windows.Forms.Button();
            this.panelMain = new System.Windows.Forms.Panel();
            this.panelMain.SuspendLayout();
            this.SuspendLayout();
            // 
            // labelIdleSince
            // 
            this.labelIdleSince.Anchor = System.Windows.Forms.AnchorStyles.Top;
            this.labelIdleSince.AutoSize = true;
            this.labelIdleSince.Font = new System.Drawing.Font("Lucida Sans Unicode", 10.5F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Pixel, ((byte)(186)));
            this.labelIdleSince.Location = new System.Drawing.Point(13, 15);
            this.labelIdleSince.Name = "labelIdleSince";
            this.labelIdleSince.Size = new System.Drawing.Size(189, 15);
            this.labelIdleSince.TabIndex = 0;
            this.labelIdleSince.Text = "You have been idle since 12:34:56";
            // 
            // labelIdleDuration
            // 
            this.labelIdleDuration.AutoSize = true;
            this.labelIdleDuration.Font = new System.Drawing.Font("Lucida Sans Unicode", 10.5F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Pixel, ((byte)(186)));
            this.labelIdleDuration.Location = new System.Drawing.Point(72, 40);
            this.labelIdleDuration.Name = "labelIdleDuration";
            this.labelIdleDuration.Size = new System.Drawing.Size(69, 15);
            this.labelIdleDuration.TabIndex = 1;
            this.labelIdleDuration.Text = "(5 minutes)";
            // 
            // buttonKeepTime
            // 
            this.buttonKeepTime.Font = new System.Drawing.Font("Lucida Sans Unicode", 10.5F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Pixel, ((byte)(186)));
            this.buttonKeepTime.Location = new System.Drawing.Point(13, 75);
            this.buttonKeepTime.Name = "buttonKeepTime";
            this.buttonKeepTime.Size = new System.Drawing.Size(75, 23);
            this.buttonKeepTime.TabIndex = 2;
            this.buttonKeepTime.Text = "Keep time";
            this.buttonKeepTime.UseVisualStyleBackColor = true;
            this.buttonKeepTime.Click += new System.EventHandler(this.buttonKeepTime_Click);
            // 
            // buttonDiscardTime
            // 
            this.buttonDiscardTime.Anchor = ((System.Windows.Forms.AnchorStyles)((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Right)));
            this.buttonDiscardTime.DialogResult = System.Windows.Forms.DialogResult.Cancel;
            this.buttonDiscardTime.Font = new System.Drawing.Font("Lucida Sans Unicode", 10.5F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Pixel, ((byte)(186)));
            this.buttonDiscardTime.Location = new System.Drawing.Point(126, 75);
            this.buttonDiscardTime.Name = "buttonDiscardTime";
            this.buttonDiscardTime.Size = new System.Drawing.Size(75, 23);
            this.buttonDiscardTime.TabIndex = 3;
            this.buttonDiscardTime.Text = "Discard time";
            this.buttonDiscardTime.UseVisualStyleBackColor = true;
            this.buttonDiscardTime.Click += new System.EventHandler(this.buttonDiscardTime_Click);
            // 
            // panelMain
            // 
            this.panelMain.Controls.Add(this.labelIdleSince);
            this.panelMain.Controls.Add(this.buttonDiscardTime);
            this.panelMain.Controls.Add(this.labelIdleDuration);
            this.panelMain.Controls.Add(this.buttonKeepTime);
            this.panelMain.Dock = System.Windows.Forms.DockStyle.Fill;
            this.panelMain.Location = new System.Drawing.Point(0, 0);
            this.panelMain.Name = "panelMain";
            this.panelMain.Size = new System.Drawing.Size(214, 116);
            this.panelMain.TabIndex = 4;
            // 
            // IdleNotificationWindowController
            // 
            this.AcceptButton = this.buttonKeepTime;
            this.AutoScaleDimensions = new System.Drawing.SizeF(6F, 13F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.CancelButton = this.buttonDiscardTime;
            this.ClientSize = new System.Drawing.Size(214, 116);
            this.Controls.Add(this.panelMain);
            this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
            this.MaximizeBox = false;
            this.MaximumSize = new System.Drawing.Size(214, 147);
            this.MinimizeBox = false;
            this.MinimumSize = new System.Drawing.Size(214, 147);
            this.Name = "IdleNotificationWindowController";
            this.TopMost = true;
            this.FormClosing += new System.Windows.Forms.FormClosingEventHandler(this.IdleNotificationWindowController_FormClosing);
            this.Load += new System.EventHandler(this.IdleNotificationWindowController_Load);
            this.panelMain.ResumeLayout(false);
            this.panelMain.PerformLayout();
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.Label labelIdleSince;
        private System.Windows.Forms.Label labelIdleDuration;
        private System.Windows.Forms.Button buttonKeepTime;
        private System.Windows.Forms.Button buttonDiscardTime;
        private System.Windows.Forms.Panel panelMain;
    }
}