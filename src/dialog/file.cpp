/*
    Copyright 2008 Brain Research Institute, Melbourne, Australia

    Written by J-Donald Tournier, 27/06/08.

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <QLayout>
#include <QStyle>
#include <QPushButton>
#include <QTreeView>
#include <QSplitter>
#include <QLineEdit>
#include <QLabel>
#include <QTimer>
#include <QSortFilterProxyModel>

#include "dialog/file.h"
#include "image/format/list.h"
#include "file/path.h"
#include "file/dicom/quick_scan.h"
#include "file/dicom/image.h"
#include "file/dicom/series.h"
#include "file/dicom/study.h"
#include "file/dicom/patient.h"
#include "file/dicom/mapper.h"
#include "app.h"

#define FILE_DIALOG_BUSY_INTERVAL 0.1

// TODO: volumes under Windows

namespace MR {
  namespace Dialog {

    std::string File::cwd (Path::cwd());
    QPoint File::window_position (-1, -1);
    QSize File::window_size (500, 500);

    /*************************************************
     *                 FolderModel                   *
     *************************************************/

    void FolderModel::add_entries (const QStringList& more)
    {
      if (more.empty()) return;
      beginInsertRows (QModelIndex(), list.size(), list.size() + more.size() - 1);
      list += more;
      endInsertRows();
      emit layoutChanged();
    }

    void FolderModel::clear ()
    {
      if (list.size()) {
        beginRemoveRows (QModelIndex(), 0, list.size()-1);
        list.clear();
        endRemoveRows();
      }
      emit layoutChanged();
    }

    int FolderModel::rowCount (const QModelIndex &parent) const { return (list.size()); }

    QVariant FolderModel::data (const QModelIndex &index, int role) const 
    { 
      if (index.isValid()) 
        if (index.row() < list.size() && role == Qt::DisplayRole) 
          return (list.at (index.row()));
      return (QVariant());
    }

    QVariant FolderModel::headerData (int section, Qt::Orientation orientation, int role) const 
    {
      if (role != Qt::DisplayRole) return QVariant();
      return (QString ("Folders")); 
    }




    /*************************************************
     *                 FileModel                     *
     *************************************************/


    void FileModel::add_entries (const std::vector<std::string>& more)
    {
      size_t prev_num_dicom_series = num_dicom_series;
      num_dicom_series = 0;
      for (size_t pat = 0; pat < dicom_tree.size(); ++pat) 
        for (size_t study = 0; study < dicom_tree[pat]->size(); ++study) 
          num_dicom_series += (*dicom_tree[pat])[study]->size(); 

      if (more.empty() && prev_num_dicom_series == num_dicom_series) return;
      beginInsertRows (QModelIndex(), list.size() + prev_num_dicom_series, list.size() + more.size() + num_dicom_series - 1);
      list.insert (list.end(), more.begin(), more.end());
      std::sort (list.begin(), list.end());
      endInsertRows();
      emit layoutChanged();
    }

    void FileModel::clear ()
    {
      if (list.size() + num_dicom_series) {
        beginRemoveRows (QModelIndex(), 0, list.size()-1);
        dicom_tree.clear();
        list.clear();
        num_dicom_series = 0;
        endRemoveRows();
      }
      emit layoutChanged();
    }

    int FileModel::rowCount (const QModelIndex &parent) const { return (list.size() + num_dicom_series); }

    QVariant FileModel::data (const QModelIndex &index, int role) const 
    { 
      if (index.isValid()) {
        if (role == Qt::DisplayRole) {
          if (index.row() < int (num_dicom_series)) {
            const MR::File::Dicom::Series& series (*get_dicom_series (index.row()));
            return (QVariant (std::string (
                    "[" + str (series.number) + "] " + series.name + ": " + str (series.size()) + " images (" + series.study->patient->name 
            + " - " + MR::File::Dicom::format_date (series.date) + ")").c_str()));
          }
          else if (index.row() < int (num_dicom_series + list.size())) 
            return (QVariant (list[index.row()-num_dicom_series].c_str()));
        }
        else if (role == Qt::ToolTipRole) {
          if (index.row() < int (num_dicom_series)) {
            const MR::File::Dicom::Series& series (*get_dicom_series (index.row()));
            return (QVariant (std::string (
                    "patient: " + series.study->patient->name 
                    + "\n\tDOB: " + MR::File::Dicom::format_date (series.study->patient->DOB) + "\n\tID: " + series.study->patient->ID
                    + "\nstudy: " + series.study->name + "\n\tdate: " + MR::File::Dicom::format_date (series.study->date) + " at "
                    + MR::File::Dicom::format_time (series.study->time) +"\n\tID: " + series.study->ID
                    + "\nseries " + str (series.number) + ": " + series.name + "\n\t" + str (series.size()) + " images\n\tdate: " 
                    + MR::File::Dicom::format_date (series.date) + " at " + MR::File::Dicom::format_time (series.time)).c_str()));
          }
        }
        
      }
      return (QVariant());
    }

    QVariant FileModel::headerData (int section, Qt::Orientation orientation, int role) const 
    {
      if (role != Qt::DisplayRole) return QVariant();
      return (QString ("Files")); 
    }


    inline bool FileModel::check_image (const std::string& path)
    {
      for (const char** ext = Image::Format::known_extensions; *ext; ext++) 
        if (Path::has_suffix (path, *ext)) return (true);
      check_dicom (path);
      return (false);
    }


    void FileModel::check_dicom (const std::string& path)
    {
      MR::File::Dicom::QuickScan reader;
      if (reader.read (path)) return;

      RefPtr<MR::File::Dicom::Patient> patient = dicom_tree.find (reader.patient, reader.patient_ID, reader.patient_DOB);
      RefPtr<MR::File::Dicom::Study> study = patient->find (reader.study, reader.study_ID, reader.study_date, reader.study_time);
      RefPtr<MR::File::Dicom::Series> series = study->find (reader.series, reader.series_number, reader.modality, reader.series_date, reader.series_time);

      RefPtr<MR::File::Dicom::Image> image (new MR::File::Dicom::Image);
      image->filename = path;
      image->series = series.get();
      image->sequence_name = reader.sequence;
      series->push_back (image);
      emit layoutChanged(); 
    }


    RefPtr<MR::File::Dicom::Series> FileModel::get_dicom_series (size_t index) const
    {
      assert (index < num_dicom_series);
      size_t i = 0;
      for (size_t pat = 0; pat < dicom_tree.size(); ++pat) 
        for (size_t study = 0; study < dicom_tree[pat]->size(); ++study) 
          for (size_t series = 0; series < (*dicom_tree[pat])[study]->size(); ++series) 
            if (i++ == index) return ((*(*dicom_tree[pat])[study])[series]);
      return ((*(*dicom_tree[0])[0])[0]);
    }





    /*************************************************
     *                 Dialog::File                  *
     *************************************************/

    File::File (QWidget* parent, const std::string& message, bool multiselection, bool images_only) :
      QDialog (parent), 
      filter_images (images_only),
      updating_selection (false)
    {
      setWindowTitle (QString (message.c_str()));
      setModal (true);
      setSizeGripEnabled (true);
      resize (window_size);
      move (window_position);

      idle_timer = new QTimer (this);
      connect (idle_timer, SIGNAL(timeout()), this, SLOT(idle_slot()));

      QPushButton *button;
      QVBoxLayout *main_layout = new QVBoxLayout;

      QHBoxLayout *buttons_layout = new QHBoxLayout;

      button = new QPushButton (style()->standardIcon (QStyle::SP_FileDialogToParent), tr("Up"));
      connect (button, SIGNAL(clicked()), this, SLOT(up_slot()));
      buttons_layout->addWidget (button);

      button = new QPushButton (style()->standardIcon (QStyle::SP_DirHomeIcon), tr("Home"));
      connect (button, SIGNAL(clicked()), this, SLOT(home_slot()));
      buttons_layout->addWidget (button);

      button = new QPushButton (style()->standardIcon (QStyle::SP_DialogResetButton), tr("Refresh"));
      connect (button, SIGNAL(clicked()), this, SLOT(update()));
      buttons_layout->addWidget (button);

      main_layout->addLayout (buttons_layout);
      main_layout->addSpacing (12);

      QHBoxLayout* h_layout = new QHBoxLayout;
      h_layout->addWidget (new QLabel ("Path:"));
      path_entry = new QLineEdit;
      h_layout->addWidget (path_entry);
      main_layout->addLayout (h_layout);

      main_layout->addSpacing (12);

      folders = new FolderModel;
      sorted_folders = new QSortFilterProxyModel;
      sorted_folders->setSourceModel (folders);

      folder_view = new QTreeView;
      folder_view->setModel (sorted_folders);
      folder_view->setRootIsDecorated (false);
      folder_view->setSortingEnabled (true);
      folder_view->sortByColumn (0, Qt::AscendingOrder);
      folder_view->setWordWrap (false);
      folder_view->setItemsExpandable (false);
      folder_view->setSelectionMode (QAbstractItemView::SingleSelection);

      files = new FileModel;
      sorted_files = new QSortFilterProxyModel;
      sorted_files->setSourceModel (files);

      files_view = new QTreeView;
      files_view->setModel (sorted_files);
      files_view->setRootIsDecorated (false);
      files_view->setSortingEnabled (true);
      files_view->sortByColumn (0, Qt::AscendingOrder);
      files_view->setWordWrap (false);
      files_view->setItemsExpandable (false);
      files_view->setSelectionMode (multiselection ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);

      QSplitter *splitter = new QSplitter;
      splitter->setChildrenCollapsible (false);
      splitter->addWidget (folder_view);
      splitter->addWidget (files_view);
      splitter->setStretchFactor (0, 1);
      splitter->setStretchFactor (1, 3);
      main_layout->addWidget (splitter);

      main_layout->addSpacing (12);

      h_layout = new QHBoxLayout;
      h_layout->addWidget (new QLabel ("Selection:"));
      selection_entry = new QLineEdit;
      h_layout->addWidget (selection_entry);
      main_layout->addLayout (h_layout);

      main_layout->addSpacing (12);

      buttons_layout = new QHBoxLayout;
      buttons_layout->addStretch(1);

      button = new QPushButton (tr("Cancel"));
      connect (button, SIGNAL(clicked()), this, SLOT(reject()));
      buttons_layout->addWidget (button);

      ok_button = new QPushButton (tr("OK"));
      ok_button->setDefault (true);
      connect (ok_button, SIGNAL(clicked()), this, SLOT(accept()));
      buttons_layout->addWidget (ok_button);

      connect (folder_view, SIGNAL (activated(const QModelIndex&)), this, SLOT (folder_selected_slot(const QModelIndex&)));
      connect (files_view, SIGNAL (activated(const QModelIndex&)), this, SLOT (file_selected_slot(const QModelIndex&)));
      connect (files_view->selectionModel(), SIGNAL (selectionChanged (const QItemSelection&, const QItemSelection&)),
            selection_entry, SLOT (clear()));

      main_layout->addLayout (buttons_layout);
      setLayout (main_layout);

      update();
    }





    File::~File ()
    {
      window_position = pos();
      window_size = size();
    }




    void File::update () 
    {
      ok_button->setEnabled (false);
      setCursor (Qt::WaitCursor);
      folders->clear();
      files->clear();
      selection_entry->clear();
      path_entry->setText (cwd.c_str());
      dir = new Path::Dir (cwd);

      QStringList folder_list;
      std::string entry;
      while (!(entry = dir->read_name()).empty()) {
        if (entry[0] == '.') continue;
        if (Path::is_dir (Path::join (cwd, entry))) 
          folder_list += entry.c_str();
      }
      folders->add_entries (folder_list);

      dir->rewind();
      idle_timer->start();
      elapsed_timer.start();
    }


    void File::up_slot ()
    {
      cwd = Path::dirname (cwd);
      update();
    }

    void File::home_slot ()
    {
      cwd = Path::home();
      update();
    }

    void File::folder_selected_slot (const QModelIndex& index)
    {
      cwd = Path::join (cwd, folders->name (sorted_folders->mapToSource (index).row()));
      update();
    }

    void File::file_selected_slot (const QModelIndex& index) { accept(); }




    void File::idle_slot ()
    {
      assert (dir);
      std::vector<std::string> file_list;
      std::string entry;
      while (elapsed_timer.elapsed() < FILE_DIALOG_BUSY_INTERVAL) {
        entry = get_next_file();
        if (entry.empty()) {
          idle_timer->stop();
          files->add_entries (file_list);
          unsetCursor ();
          ok_button->setEnabled (true);
          dir = NULL;
          return;
        }

        if (filter_images) {
          if (files->check_image (Path::join (cwd, entry)))
            file_list.push_back (entry);
        }
        else file_list.push_back (entry);
      }

      elapsed_timer.start();
      files->add_entries (file_list);
    }








    void File::get_selection (std::vector<std::string>& filenames)
    {
      assert (!filter_images);
      if (selection_entry->text().size()) 
        filenames.push_back (selection_entry->text().toAscii().constData());
      else {
        QModelIndexList indexes = files_view->selectionModel()->selectedIndexes();
        if (indexes.size()) {
          QModelIndex index;
          foreach (index, indexes) 
            filenames.push_back (Path::join (cwd, files->name (index.row())));
        }
      }
    }





    void File::get_images (VecPtr<Image::Header>& images)
    {
      assert (filter_images);
      QModelIndexList indexes = files_view->selectionModel()->selectedIndexes();
      QModelIndex index;
      foreach (index, indexes) {
        if (files->is_file (index.row())) {
          images.push_back (new Image::Header (Image::Header::open (Path::join (cwd, files->name (index.row())))));
        }
        else {
          std::vector< RefPtr<MR::File::Dicom::Series> > series;
          series.push_back (RefPtr<MR::File::Dicom::Series> (files->get_dicom_series (index.row())));
          Image::Header* H = new Image::Header;
          dicom_to_mapper (*H, series);
          images.push_back (H);
        }
      }
    }



  }
}


